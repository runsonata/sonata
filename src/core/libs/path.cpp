// @sonata/path: Lexical path manipulation
//
//     local path = require("@sonata/path")
//
// path.* uses the style of the host. path.posix.* and path.win32.* are the same
// functions for a fixed style; on the other style's host they cannot know a
// working directory, so resolve() leaves relative results relative.
// win32 comparisons are ASCII case-insensitive.
//
// Variables
//     path.sep                               --> "/"       ("\\" on Windows)
//     path.delimiter                         --> ":"       (";"  on Windows)
//     path.style                             --> "posix"   ("win32" on Windows)
//     path.posix, path.win32                 --> the same API for one fixed style
//
// Building and normalizing (results never end in a separator, except roots)
//     path.normalize("/a/./b//c/..")         --> "/a/b"
//     path.join("a", "b/", "../c")           --> "a/c"          (no arguments: ".")
//     path.resolve("src", "../lib")          --> "<cwd>/lib"    (right to left, until absolute)
//     path.relative("/a/b", "/a/c/d")        --> "../c/d"       (same path: ".")
//     path.isInside("/a/b/c", "/a/b")        --> true           (also true for equal paths)
//
// Taking apart (textual: nothing is normalized or looked up)
//     path.dirname("/a/b/c.txt")             --> "/a/b"         ("a": ".", "/": "/")
//     path.basename("/a/b/c.txt")            --> "c.txt"
//     path.basename("/a/b/c.txt", ".txt")    --> "c"
//     path.extname("/a/b/c.tar.gz")          --> ".gz"
//     path.stem("/a/b/c.tar.gz")             --> "c.tar"
//     path.withExtension("a/b.txt", "md")    --> "a/b.md"       ("" removes the extension)
//     path.isAbsolute("/a")                  --> true
//     path.components("/a/b/../c")           --> { "/", "a", "b", "..", "c" }
//     path.parse("/a/b.txt")                 --> { root = "/", dir = "/a", base = "b.txt",
//                                                  ext = ".txt", name = "b" }
//     path.format({ dir = "/a", name = "b", ext = "txt" })   --> "/a/b.txt"
//
// Conversion and PATH-style lists
//     path.toPosix("a\\b")                   --> "a/b"          (win32 style only)
//     path.toNative("a/b")                   --> "a\\b"         (win32 style only)
//     path.splitList("/bin:/usr/bin")        --> { "/bin", "/usr/bin" }  (empty entries are dropped)
//     path.joinList({ "/bin", "/usr/bin" })  --> "/bin:/usr/bin"

#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#if __has_include(<version>)
#include <version>
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

// Rule of thumb (same as example.cpp): validate ALL arguments first, then
// create C++ objects. Errors that can only be found once C++ objects exist
// (the working directory being unavailable) are reported by pushing the
// message and returning kFailed; finish() raises it after the objects are gone.

constexpr int kFailed = -1;

int fail(lua_State* L, const std::string& message) {
    lua_pushlstring(L, message.data(), message.size());
    return kFailed;
}

int finish(lua_State* L, int results) {
    if (results < 0) {
        luaL_error(L, "%s", lua_tostring(L, -1));
    }
    return results;
}

std::string_view checkText(lua_State* L, int arg) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, arg, &length);
    return std::string_view(text, length);
}

void pushString(lua_State* L, std::string_view text) {
    lua_pushlstring(L, text.data(), text.size());
}

void setStringField(lua_State* L, const char* key, std::string_view text) {
    pushString(L, text);
    lua_setfield(L, -2, key);
}

char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string toUtf8(const std::filesystem::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string text = p.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return p.u8string();
#endif
}

// Styles ///////////////////////////////

// What comes before the first component of a path.
//   length   characters of the input the root occupies (including separators)
//   device   normalized "C:", "\\server\share", "\\?\C:" ... (empty on posix)
//   absolute true if the path is anchored ("/", "\", "C:\"; not "C:")
struct Root {
    std::size_t length = 0;
    std::string device;
    bool absolute = false;
};

struct Posix {
    static constexpr char kSep = '/';
    static constexpr char kDelimiter = ':';
    static constexpr const char* kStyle = "posix";
    static constexpr bool kCaseInsensitive = false;

    static bool isSep(char c) { return c == '/'; }

    static Root parseRoot(std::string_view p) {
        Root root;
        while (root.length < p.size() && p[root.length] == '/') {
            ++root.length;
        }
        root.absolute = root.length > 0;
        return root;
    }
};

struct Win32 {
    static constexpr char kSep = '\\';
    static constexpr char kDelimiter = ';';
    static constexpr const char* kStyle = "win32";
    static constexpr bool kCaseInsensitive = true;

    static bool isSep(char c) { return c == '/' || c == '\\'; }

    static bool isDriveLetter(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    static std::size_t skipSeps(std::string_view p, std::size_t i) {
        while (i < p.size() && isSep(p[i])) {
            ++i;
        }
        return i;
    }

    static std::size_t skipNonSeps(std::string_view p, std::size_t i) {
        while (i < p.size() && !isSep(p[i])) {
            ++i;
        }
        return i;
    }

    // Reads "server<seps>share" starting at `start`.
    static bool readShare(std::string_view p, std::size_t start, std::string& server,
                          std::string& share, std::size_t& end) {
        const std::size_t serverEnd = skipNonSeps(p, start);
        const std::size_t shareStart = skipSeps(p, serverEnd);
        const std::size_t shareEnd = skipNonSeps(p, shareStart);
        if (serverEnd == start || shareStart == serverEnd || shareEnd == shareStart) {
            return false;
        }
        server.assign(p.substr(start, serverEnd - start));
        share.assign(p.substr(shareStart, shareEnd - shareStart));
        end = shareEnd;
        return true;
    }

    // \\?\C:\..., \\?\UNC\server\share\..., \\.\device\...
    static Root parseDevicePath(std::string_view p) {
        Root root;
        root.absolute = true;

        const std::string prefix = std::string("\\\\") + p[2] + "\\";
        const std::size_t pos = 4;

        if (p.size() >= pos + 2 && isDriveLetter(p[pos]) && p[pos + 1] == ':') {
            root.device = prefix + std::string(p.substr(pos, 2));
            root.length = skipSeps(p, pos + 2);
            return root;
        }

        if (p.size() >= pos + 3 && asciiLower(p[pos]) == 'u' && asciiLower(p[pos + 1]) == 'n' &&
            asciiLower(p[pos + 2]) == 'c' && (p.size() == pos + 3 || isSep(p[pos + 3]))) {
            std::string server;
            std::string share;
            std::size_t end = 0;
            if (readShare(p, skipSeps(p, pos + 3), server, share, end)) {
                root.device = prefix + "UNC\\" + server + "\\" + share;
                root.length = skipSeps(p, end);
                return root;
            }
        }

        const std::size_t end = skipNonSeps(p, pos);
        root.device = prefix + std::string(p.substr(pos, end - pos));
        root.length = skipSeps(p, end);
        return root;
    }

    static Root parseRoot(std::string_view p) {
        Root root;

        if (p.size() >= 4 && isSep(p[0]) && isSep(p[1]) && (p[2] == '?' || p[2] == '.') &&
            isSep(p[3])) {
            return parseDevicePath(p);
        }

        // "C:" (relative to the drive's directory) or "C:\" (absolute)
        if (p.size() >= 2 && isDriveLetter(p[0]) && p[1] == ':') {
            root.device.assign(p.substr(0, 2));
            root.length = skipSeps(p, 2);
            root.absolute = root.length > 2;
            return root;
        }

        // UNC: two separators, server, share
        if (p.size() >= 2 && isSep(p[0]) && isSep(p[1])) {
            std::string server;
            std::string share;
            std::size_t end = 0;
            if (readShare(p, 2, server, share, end)) {
                root.device = "\\\\" + server + "\\" + share;
                root.length = skipSeps(p, end);
                root.absolute = true;
                return root;
            }
        }

        // \ (the root of the current drive)
        if (!p.empty() && isSep(p[0])) {
            root.length = skipSeps(p, 0);
            root.absolute = true;
        }
        return root;
    }
};

#ifdef _WIN32
using Host = Win32;
#else
using Host = Posix;
#endif

template <typename S>
bool sameText(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = S::kCaseInsensitive ? asciiLower(a[i]) : a[i];
        const char y = S::kCaseInsensitive ? asciiLower(b[i]) : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

// Structural helpers ///////////////////////////////

// A path as device + anchored flag + components. "." and empty components are
// dropped while parsing; ".." is kept until collapse() is called.
struct Parsed {
    std::string device;
    bool absolute = false;
    std::vector<std::string> parts;
};

template <typename S>
Parsed parse(std::string_view p) {
    const Root root = S::parseRoot(p);

    Parsed out;
    out.device = root.device;
    out.absolute = root.absolute;

    std::size_t i = root.length;
    while (i < p.size()) {
        std::size_t j = i;
        while (j < p.size() && !S::isSep(p[j])) {
            ++j;
        }
        const bool isDot = (j - i == 1 && p[i] == '.');
        if (j > i && !isDot) {
            out.parts.emplace_back(p.substr(i, j - i));
        }
        i = j + 1;
    }
    return out;
}

// Resolves ".." lexically. Above the root of an absolute path they vanish; in
// a relative path leading ones are kept.
std::vector<std::string> collapse(std::vector<std::string> parts, bool absolute) {
    std::vector<std::string> out;
    out.reserve(parts.size());
    for (std::string& part : parts) {
        if (part == "..") {
            if (!out.empty() && out.back() != "..") {
                out.pop_back();
            } else if (!absolute) {
                out.push_back("..");
            }
        } else {
            out.push_back(std::move(part));
        }
    }
    return out;
}

template <typename S>
std::string rootText(const Root& root) {
    std::string out = root.device;
    if (root.absolute) {
        out.push_back(S::kSep);
    }
    return out;
}

template <typename S>
std::string compose(const Parsed& p) {
    std::string out = p.device;
    if (p.absolute) {
        out.push_back(S::kSep);
    }
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
        if (i != 0) {
            out.push_back(S::kSep);
        }
        out += p.parts[i];
    }
    if (p.parts.empty() && !p.absolute) {
        out.push_back('.'); // "" -> ".", "C:" -> "C:."
    }
    return out;
}

template <typename S>
std::string normalizeText(std::string_view text) {
    Parsed p = parse<S>(text);
    p.parts = collapse(std::move(p.parts), p.absolute);
    return compose<S>(p);
}

// Where the pieces of the original text are, for the textual functions.
struct TextSplit {
    Root root;
    std::size_t dirEnd = 0;    // directory part is [root.length, dirEnd)
    std::size_t baseStart = 0; // base name is [baseStart, baseEnd)
    std::size_t baseEnd = 0;
};

template <typename S>
TextSplit splitText(std::string_view p) {
    TextSplit s;
    s.root = S::parseRoot(p);

    std::size_t end = p.size();
    while (end > s.root.length && S::isSep(p[end - 1])) {
        --end;
    }
    std::size_t start = end;
    while (start > s.root.length && !S::isSep(p[start - 1])) {
        --start;
    }
    std::size_t dirEnd = start;
    while (dirEnd > s.root.length && S::isSep(p[dirEnd - 1])) {
        --dirEnd;
    }

    s.baseStart = start;
    s.baseEnd = end;
    s.dirEnd = dirEnd;
    return s;
}

std::string_view baseOf(std::string_view p, const TextSplit& s) {
    return p.substr(s.baseStart, s.baseEnd - s.baseStart);
}

// Position of the extension's dot inside a base name, or base.size() if none.
// ".bashrc" and ".." have no extension; "a." has ".".
std::size_t extStart(std::string_view base) {
    if (base == "..") {
        return base.size();
    }
    const std::size_t dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return base.size();
    }
    return dot;
}

template <typename S>
std::string dirnameOf(std::string_view p, bool emptyWhenNone) {
    const TextSplit s = splitText<S>(p);
    if (s.dirEnd > s.root.length) {
        std::string out = rootText<S>(s.root);
        out.append(p.substr(s.root.length, s.dirEnd - s.root.length));
        return out;
    }
    if (s.root.length > 0) {
        return rootText<S>(s.root);
    }
    return emptyWhenNone ? std::string() : std::string(".");
}

// resolve / relative ///////////////////////////////

bool currentDirectory(std::string& out, std::string& error) {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    out = toUtf8(cwd);
    return true;
}

// Same algorithm as Node's path.resolve: go from the last argument to the
// first until the path is anchored (and, on win32, has a drive or share), then
// fall back to the working directory. Only the host style knows a working
// directory; for the other style a relative result stays relative.
template <typename S>
bool resolveParsed(const std::vector<std::string_view>& args, Parsed& out, std::string& error) {
    constexpr bool kHasDevices = std::is_same_v<S, Win32>;

    std::string device;
    bool absolute = false;
    std::vector<std::vector<std::string>> tails; // collected right to left

    const auto complete = [&] { return absolute && (!kHasDevices || !device.empty()); };

    const auto consume = [&](std::string_view text) {
        if (text.empty()) {
            return;
        }
        Parsed p = parse<S>(text);
        if (!p.device.empty() && !device.empty() && !sameText<S>(p.device, device)) {
            return; // a different drive: its path says nothing about ours
        }
        if (device.empty() && !p.device.empty()) {
            device = p.device;
        }
        if (!absolute) {
            tails.push_back(std::move(p.parts));
            absolute = p.absolute;
        }
    };

    for (std::size_t i = args.size(); i-- > 0 && !complete();) {
        consume(args[i]);
    }

    if constexpr (std::is_same_v<S, Host>) {
        if (!complete()) {
            std::string cwd;
            if (!currentDirectory(cwd, error)) {
                return false;
            }
            const Parsed base = parse<S>(cwd);
            if (!absolute && !device.empty() && !base.device.empty() &&
                !sameText<S>(base.device, device)) {
                absolute = true; // "D:foo" while cwd is on C: -> "D:\foo"
            } else {
                consume(cwd);
            }
        }
    }

    std::vector<std::string> parts;
    for (auto it = tails.rbegin(); it != tails.rend(); ++it) {
        for (std::string& part : *it) {
            parts.push_back(std::move(part));
        }
    }

    out.device = device;
    out.absolute = absolute;
    out.parts = collapse(std::move(parts), absolute);
    return true;
}

template <typename S>
bool relativeText(std::string_view from, std::string_view to, std::string& result,
                  std::string& error) {
    Parsed a;
    Parsed b;
    if (!resolveParsed<S>({from}, a, error) || !resolveParsed<S>({to}, b, error)) {
        return false;
    }

    // Nothing to be relative to: hand back the target itself.
    if (a.absolute != b.absolute || !sameText<S>(a.device, b.device)) {
        result = compose<S>(b);
        return true;
    }

    std::size_t common = 0;
    while (common < a.parts.size() && common < b.parts.size() &&
           sameText<S>(a.parts[common], b.parts[common])) {
        ++common;
    }

    std::vector<std::string> out;
    for (std::size_t i = common; i < a.parts.size(); ++i) {
        if (a.parts[i] == "..") {
            error = "cannot get from a base that climbs above its start without a working directory";
            return false;
        }
        out.emplace_back("..");
    }
    for (std::size_t i = common; i < b.parts.size(); ++i) {
        out.push_back(b.parts[i]);
    }

    Parsed rel;
    rel.parts = std::move(out);
    result = compose<S>(rel); // empty -> "."
    return true;
}

// Bindings

// path.normalize(path: string): string
template <typename S>
int pathNormalize(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    pushString(L, normalizeText<S>(text));
    return 1;
}

// path.join(...: string): string
template <typename S>
int pathJoin(lua_State* L) {
    const int argc = lua_gettop(L);
    for (int i = 1; i <= argc; ++i) {
        luaL_checkstring(L, i);
    }

    std::string joined;
    for (int i = 1; i <= argc; ++i) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, i, &length);
        if (length == 0) {
            continue;
        }
        if (!joined.empty()) {
            joined.push_back(S::kSep);
        }
        joined.append(text, length);
    }

    pushString(L, joined.empty() ? std::string(".") : normalizeText<S>(joined));
    return 1;
}

template <typename S>
int resolveImpl(lua_State* L, int argc) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i <= argc; ++i) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, i, &length);
        args.emplace_back(text, length);
    }

    Parsed resolved;
    std::string error;
    if (!resolveParsed<S>(args, resolved, error)) {
        return fail(L, "path.resolve: cannot get the current directory: " + error);
    }

    pushString(L, compose<S>(resolved));
    return 1;
}

// path.resolve(...: string): string
template <typename S>
int pathResolve(lua_State* L) {
    const int argc = lua_gettop(L);
    for (int i = 1; i <= argc; ++i) {
        luaL_checkstring(L, i);
    }
    return finish(L, resolveImpl<S>(L, argc));
}

template <typename S>
int relativeImpl(lua_State* L, std::string_view from, std::string_view to) {
    std::string result;
    std::string error;
    if (!relativeText<S>(from, to, result, error)) {
        return fail(L, "path.relative: " + error);
    }
    pushString(L, result);
    return 1;
}

// path.relative(from: string, to: string): string
template <typename S>
int pathRelative(lua_State* L) {
    const std::string_view from = checkText(L, 1);
    const std::string_view to = checkText(L, 2);
    return finish(L, relativeImpl<S>(L, from, to));
}

template <typename S>
int insideImpl(lua_State* L, std::string_view child, std::string_view base) {
    Parsed a;
    Parsed b;
    std::string error;
    if (!resolveParsed<S>({child}, a, error) || !resolveParsed<S>({base}, b, error)) {
        return fail(L, "path.isInside: cannot get the current directory: " + error);
    }

    bool inside = a.absolute == b.absolute && sameText<S>(a.device, b.device) &&
                  b.parts.size() <= a.parts.size();
    for (std::size_t i = 0; inside && i < b.parts.size(); ++i) {
        inside = sameText<S>(a.parts[i], b.parts[i]);
    }
    for (std::size_t i = b.parts.size(); inside && i < a.parts.size(); ++i) {
        inside = a.parts[i] != ".."; // only possible for relative paths
    }

    lua_pushboolean(L, inside ? 1 : 0);
    return 1;
}

// path.isInside(path: string, directory: string): boolean
template <typename S>
int pathIsInside(lua_State* L) {
    const std::string_view child = checkText(L, 1);
    const std::string_view base = checkText(L, 2);
    return finish(L, insideImpl<S>(L, child, base));
}

// path.isAbsolute(path: string): boolean
template <typename S>
int pathIsAbsolute(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    lua_pushboolean(L, S::parseRoot(text).absolute ? 1 : 0);
    return 1;
}

// path.dirname(path: string): string
template <typename S>
int pathDirname(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    pushString(L, dirnameOf<S>(text, false));
    return 1;
}

// path.basename(path: string, suffix: string?): string
template <typename S>
int pathBasename(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    std::size_t suffixLength = 0;
    const char* suffix = luaL_optlstring(L, 2, "", &suffixLength);

    std::string_view base = baseOf(text, splitText<S>(text));
    if (suffixLength != 0 && base.size() > suffixLength &&
        sameText<S>(base.substr(base.size() - suffixLength),
                    std::string_view(suffix, suffixLength))) {
        base.remove_suffix(suffixLength);
    }

    pushString(L, base);
    return 1;
}

// path.extname(path: string): string
template <typename S>
int pathExtname(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    const std::string_view base = baseOf(text, splitText<S>(text));
    pushString(L, base.substr(extStart(base)));
    return 1;
}

// path.stem(path: string): string
template <typename S>
int pathStem(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    const std::string_view base = baseOf(text, splitText<S>(text));
    pushString(L, base.substr(0, extStart(base)));
    return 1;
}

// path.withExtension(path: string, extension: string): string
template <typename S>
int pathWithExtension(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    const std::string_view ext = checkText(L, 2);
    for (const char c : ext) {
        luaL_argcheck(L, !S::isSep(c), 2, "extension must not contain path separators");
    }

    const TextSplit s = splitText<S>(text);
    const std::string_view base = baseOf(text, s);

    std::string out;
    if (base.empty() || base == "." || base == "..") {
        out.assign(text); // no file name to change
    } else {
        out.assign(text.substr(0, s.baseStart));
        out.append(base.substr(0, extStart(base)));
        if (!ext.empty() && ext.front() != '.') {
            out.push_back('.');
        }
        out.append(ext);
    }

    pushString(L, out);
    return 1;
}

// path.parse(path: string): { root: string, dir: string, base: string, ext: string, name: string }
// Like Node: dir is "" when the path has no directory part.
template <typename S>
int pathParse(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    const TextSplit s = splitText<S>(text);
    const std::string_view base = baseOf(text, s);
    const std::size_t dot = extStart(base);

    lua_createtable(L, 0, 5);
    setStringField(L, "root", rootText<S>(s.root));
    setStringField(L, "dir", dirnameOf<S>(text, true));
    setStringField(L, "base", base);
    setStringField(L, "ext", base.substr(dot));
    setStringField(L, "name", base.substr(0, dot));
    return 1;
}

// path.format(parts: { root: string?, dir: string?, base: string?, name: string?, ext: string? }): string
template <typename S>
int pathFormat(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    constexpr const char* kFields[5] = {"root", "dir", "base", "name", "ext"};
    std::string_view values[5];
    for (int i = 0; i < 5; ++i) {
        lua_getfield(L, 1, kFields[i]); // stays on the stack, which keeps the string alive
        if (lua_isnoneornil(L, -1)) {
            continue;
        }
        if (lua_type(L, -1) != LUA_TSTRING) {
            luaL_error(L, "field '%s' must be a string", kFields[i]);
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        values[i] = std::string_view(text, length);
    }

    const std::string_view root = values[0];
    const std::string_view dirIn = values[1];
    const std::string_view baseIn = values[2];
    const std::string_view name = values[3];
    const std::string_view extIn = values[4];

    std::string base;
    if (!baseIn.empty()) {
        base.assign(baseIn);
    } else {
        base.assign(name);
        if (!extIn.empty()) {
            if (extIn.front() != '.') {
                base.push_back('.');
            }
            base.append(extIn);
        }
    }

    const std::string_view dir = dirIn.empty() ? root : dirIn;
    std::string out(dir);
    if (!dir.empty() && !base.empty() && dir != root && !S::isSep(dir.back())) {
        out.push_back(S::kSep);
    }
    out.append(base);

    pushString(L, out);
    return 1;
}

// path.components(path: string): { string }
// The root (if any) comes first. "." and empty components are dropped, ".." is kept.
template <typename S>
int pathComponents(lua_State* L) {
    const std::string_view text = checkText(L, 1);
    const Parsed p = parse<S>(text);

    std::vector<std::string> out;
    if (p.absolute || !p.device.empty()) {
        Root root;
        root.device = p.device;
        root.absolute = p.absolute;
        out.push_back(rootText<S>(root));
    }
    out.insert(out.end(), p.parts.begin(), p.parts.end());

    lua_createtable(L, static_cast<int>(out.size()), 0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        pushString(L, out[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// path.toPosix(path: string): string   every separator becomes "/"
template <typename S>
int pathToPosix(lua_State* L) {
    std::string text(checkText(L, 1));
    for (char& c : text) {
        if (S::isSep(c)) {
            c = '/';
        }
    }
    pushString(L, text);
    return 1;
}

// path.toNative(path: string): string   every separator becomes the style's own
template <typename S>
int pathToNative(lua_State* L) {
    std::string text(checkText(L, 1));
    for (char& c : text) {
        if (S::isSep(c)) {
            c = S::kSep;
        }
    }
    pushString(L, text);
    return 1;
}

// path.splitList(list: string): { string }
template <typename S>
int pathSplitList(lua_State* L) {
    const std::string_view text = checkText(L, 1);

    std::vector<std::string_view> items;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(S::kDelimiter, start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            items.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }

    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        pushString(L, items[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// path.joinList(paths: { string }): string
template <typename S>
int pathJoinList(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const int count = lua_objlen(L, 1);

    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 1, i);
        if (lua_type(L, -1) != LUA_TSTRING) {
            luaL_error(L, "element %d must be a string", i);
        }
        lua_pop(L, 1);
    }

    std::string out;
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 1, i);
        std::size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length); // the table keeps it alive
        lua_pop(L, 1);
        if (length == 0) {
            continue;
        }
        if (!out.empty()) {
            out.push_back(S::kDelimiter);
        }
        out.append(text, length);
    }

    pushString(L, out);
    return 1;
}

template <typename S>
struct Bindings {
    static constexpr NativeFunction kFunctions[] = {
        {"normalize", pathNormalize<S>},
        {"join", pathJoin<S>},
        {"resolve", pathResolve<S>},
        {"relative", pathRelative<S>},
        {"isInside", pathIsInside<S>},
        {"isAbsolute", pathIsAbsolute<S>},
        {"dirname", pathDirname<S>},
        {"basename", pathBasename<S>},
        {"extname", pathExtname<S>},
        {"stem", pathStem<S>},
        {"withExtension", pathWithExtension<S>},
        {"parse", pathParse<S>},
        {"format", pathFormat<S>},
        {"components", pathComponents<S>},
        {"toPosix", pathToPosix<S>},
        {"toNative", pathToNative<S>},
        {"splitList", pathSplitList<S>},
        {"joinList", pathJoinList<S>},
    };
};

// Pushes a table with the functions and constants of style S.
template <typename S>
void pushStyle(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(Bindings<S>::kFunctions)) + 4);
    setFunctions(L, Bindings<S>::kFunctions);

    lua_pushlstring(L, &S::kSep, 1);
    lua_setfield(L, -2, "sep");
    lua_pushlstring(L, &S::kDelimiter, 1);
    lua_setfield(L, -2, "delimiter");
    lua_pushstring(L, S::kStyle);
    lua_setfield(L, -2, "style");
}

} // namespace

void openPath(lua_State* L) {
    pushStyle<Host>(L);

    pushStyle<Posix>(L);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "posix");

    pushStyle<Win32>(L);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "win32");
}

} // namespace sonata::lib::libs