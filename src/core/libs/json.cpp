// @sonata/json: parse, serialize and modify JSON.
//     local json = require("@sonata/json")
//
//     json.decode('{"a":[1,2,{"b":null}]}')     --> { a = { 1, 2, { b = json.null } } }
//     json.encode({ x = 1, y = { true, "s" } })  --> '{"x":1,"y":[true,"s"]}'   (keys sorted)
//     json.encode(v, { indent = 2 })             -- pretty print; indent may also be "\t" or spaces
//
//     json.get(v, "a[3].b", default)   -- read by path, `default` (nil) when missing
//     json.set(v, "a[1]", 5)           -- write in place, creating missing containers; returns v
//     json.delete(v, "a[1]")           -- remove in place (array tails shift down); returns the old value
//     json.merge(target, patch)        -- RFC 7396 merge patch, in place; json.null deletes a key
//     json.clone(v)                    -- deep copy
//     json.array(t?), json.object(t?)  -- tag a table so it always encodes as [] / {}
//     json.null
//

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

// Error rule (see example.cpp): never raise a Lua error while a std::string/vector is alive.
// The parser and encoder therefore report failures through a return value + message; the
// binding raises the error only after they went out of scope.

constexpr int kMaxDepth = 200;
constexpr char kNullTag = 0; // only the address matters: json.null is a light userdata pointing here
constexpr const char* kArrayMeta = "sonata.json.array";   // registry keys of the marker metatables
constexpr const char* kObjectMeta = "sonata.json.object";
constexpr char kEscapeIn[] = "\"\\/bfnrt";        // JSON escape letters ...
constexpr char kEscapeOut[] = "\"\\/\b\f\n\r\t";  // ... and the bytes they stand for

enum class Kind { Array, Object, Empty };

bool isNull(lua_State* L, int idx) {
    return lua_type(L, idx) == LUA_TLIGHTUSERDATA && lua_touserdata(L, idx) == &kNullTag;
}

void pushNull(lua_State* L) {
    lua_pushlightuserdata(L, const_cast<char*>(&kNullTag));
}

// Decides how the table at absolute index `t` is treated. Tags win; otherwise it is an array
// iff its keys are exactly 1..n. Untagged empty tables report Empty (the encoder writes [],
// merge treats them as objects).
Kind classify(lua_State* L, int t) {
    if (lua_getmetatable(L, t)) {
        lua_getfield(L, LUA_REGISTRYINDEX, kArrayMeta);
        lua_getfield(L, LUA_REGISTRYINDEX, kObjectMeta);
        const bool array = lua_rawequal(L, -3, -2);
        const bool object = lua_rawequal(L, -3, -1);
        lua_pop(L, 3);
        if (array) return Kind::Array;
        if (object) return Kind::Object;
    }

    const int n = lua_objlen(L, t);
    int total = 0;
    int sequence = 0;
    lua_pushnil(L);
    while (lua_next(L, t)) {
        ++total;
        if (lua_type(L, -2) == LUA_TNUMBER) {
            const double k = lua_tonumber(L, -2);
            if (k >= 1 && k <= n && k == std::floor(k)) ++sequence;
        }
        lua_pop(L, 1);
    }

    if (total == 0) return Kind::Empty;
    return (total == sequence && sequence == n) ? Kind::Array : Kind::Object;
}

// Shortest %g form (15-17 digits) that reads back identically. NaN/Infinity have no JSON form.
bool formatNumber(double v, char (&buf)[40]) {
    if (!std::isfinite(v)) return false;
    for (int digits = 15; digits <= 17; ++digits) {
        std::snprintf(buf, sizeof buf, "%.*g", digits, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return true;
}

// Parser ///////////////////////////////

struct Parser {
    lua_State* L;
    const char* s;
    std::size_t n;
    std::size_t pos = 0;
    const char* error = nullptr;
    std::string scratch{};

    bool fail(const char* message) {
        error = message;
        return false;
    }

    void skipWs() {
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) ++pos;
    }

    bool digits() { // consumes one or more digits
        const std::size_t start = pos;
        while (pos < n && s[pos] >= '0' && s[pos] <= '9') ++pos;
        return pos > start;
    }

    bool literal(std::string_view word) {
        if (std::string_view(s + pos, std::min(word.size(), n - pos)) != word) return fail("invalid literal");
        pos += word.size();
        return true;
    }

    bool hex4(unsigned& out) {
        if (pos + 4 > n) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    void appendUtf8(unsigned cp) {
        auto put = [this](unsigned v) { scratch += static_cast<char>(v); };
        if (cp < 0x80) {
            put(cp);
        } else if (cp < 0x800) {
            put(0xC0 | cp >> 6);
            put(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            put(0xE0 | cp >> 12);
            put(0x80 | (cp >> 6 & 0x3F));
            put(0x80 | (cp & 0x3F));
        } else {
            put(0xF0 | cp >> 18);
            put(0x80 | (cp >> 12 & 0x3F));
            put(0x80 | (cp >> 6 & 0x3F));
            put(0x80 | (cp & 0x3F));
        }
    }

    bool parseUnicode() { // after "\u"
        unsigned cp = 0;
        unsigned lo = 0;
        if (!hex4(cp)) return fail("invalid \\u escape");
        if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("unpaired surrogate");
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos + 2 > n || s[pos] != '\\' || s[pos + 1] != 'u') return fail("unpaired surrogate");
            pos += 2;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        appendUtf8(cp);
        return true;
    }

    bool parseString() { // pos is on the opening quote; pushes the string
        ++pos;
        scratch.clear();
        std::size_t run = pos; // start of the not-yet-copied plain bytes
        while (pos < n) {
            const unsigned char c = static_cast<unsigned char>(s[pos]);
            if (c == '"') {
                scratch.append(s + run, pos - run);
                ++pos;
                lua_pushlstring(L, scratch.data(), scratch.size());
                return true;
            }
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') {
                ++pos;
                continue;
            }

            scratch.append(s + run, pos - run);
            if (++pos >= n) break;
            const char e = s[pos++];
            const char* hit = e ? std::strchr(kEscapeIn, e) : nullptr;
            if (e == 'u') {
                if (!parseUnicode()) return false;
            } else if (hit) {
                scratch += kEscapeOut[hit - kEscapeIn];
            } else {
                return fail("invalid escape sequence");
            }
            run = pos;
        }
        return fail("unterminated string");
    }

    bool parseNumber() {
        const std::size_t start = pos;
        if (s[pos] == '-') ++pos;
        if (pos < n && s[pos] == '0') ++pos;
        else if (!digits()) return fail("invalid number");
        if (pos < n && s[pos] == '.') {
            ++pos;
            if (!digits()) return fail("invalid number");
        }
        if (pos < n && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < n && (s[pos] == '+' || s[pos] == '-')) ++pos;
            if (!digits()) return fail("invalid number");
        }
        scratch.assign(s + start, pos - start);
        const double v = std::strtod(scratch.c_str(), nullptr);
        if (!std::isfinite(v)) return fail("number out of range");
        lua_pushnumber(L, v);
        return true;
    }

    // Arrays and objects share one loop: only the key/colon part differs.
    bool parseContainer(int depth, bool object) {
        const char close = object ? '}' : ']';
        if (depth >= kMaxDepth || !lua_checkstack(L, 4)) return fail("nesting too deep");

        ++pos;
        lua_createtable(L, 0, 0);
        skipWs();
        if (pos < n && s[pos] == close) { // empty: tag it so it survives a round trip
            ++pos;
            lua_getfield(L, LUA_REGISTRYINDEX, object ? kObjectMeta : kArrayMeta);
            lua_setmetatable(L, -2);
            return true;
        }

        int count = 0;
        for (;;) {
            skipWs();
            if (object) {
                if (pos >= n || s[pos] != '"') return fail("expected string key");
                if (!parseString()) return false;
                skipWs();
                if (pos >= n || s[pos] != ':') return fail("expected ':'");
                ++pos;
            }
            if (!parseValue(depth + 1)) return false;
            if (object) lua_rawset(L, -3); // duplicate keys: the last one wins
            else lua_rawseti(L, -2, ++count);

            skipWs();
            if (pos < n && s[pos] == ',') {
                ++pos;
                continue;
            }
            if (pos < n && s[pos] == close) {
                ++pos;
                return true;
            }
            return fail(object ? "expected ',' or '}'" : "expected ',' or ']'");
        }
    }

    bool parseValue(int depth) {
        skipWs();
        if (pos >= n) return fail("unexpected end of input");
        const char c = s[pos];
        switch (c) {
        case '{': return parseContainer(depth, true);
        case '[': return parseContainer(depth, false);
        case '"': return parseString();
        case 't':
        case 'f':
            if (!literal(c == 't' ? "true" : "false")) return false;
            lua_pushboolean(L, c == 't');
            return true;
        case 'n':
            if (!literal("null")) return false;
            pushNull(L);
            return true;
        default:
            return (c == '-' || (c >= '0' && c <= '9')) ? parseNumber() : fail("unexpected character");
        }
    }
};

// json.decode(text: string): any
int jsonDecode(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);

    const char* message = nullptr;
    std::size_t at = 0;
    { // parser (and its std::string) lives only in this scope
        Parser p{L, text, length};
        if (length >= 3 && std::memcmp(text, "\xEF\xBB\xBF", 3) == 0) p.pos = 3; // skip UTF-8 BOM
        if (p.parseValue(0)) {
            p.skipWs();
            if (p.pos != length) p.fail("unexpected trailing characters");
        }
        message = p.error;
        at = p.pos;
    }

    if (message) {
        int line = 1;
        int column = 1;
        for (std::size_t i = 0; i < at && i < length; ++i) {
            if (text[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        luaL_error(L, "json.decode: %s at line %d, column %d", message, line, column);
    }
    return 1;
}

// Encoder ///////////////////////////////////////////////7

struct Encoder {
    lua_State* L;
    std::string indent; // one indentation step; empty = compact output
    std::string out{};
    const char* error = nullptr;
    const char* detail = nullptr;

    bool fail(const char* message, const char* extra = nullptr) {
        error = message;
        detail = extra;
        return false;
    }

    void newline(int depth) {
        if (indent.empty()) return;
        out += '\n';
        for (int i = 0; i < depth; ++i) out += indent;
    }

    void appendString(const char* text, std::size_t length) {
        out += '"';
        for (std::size_t i = 0; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            const char* hit = c ? std::strchr(kEscapeOut, c) : nullptr;
            if (hit && c != '/') {
                out += '\\';
                out += kEscapeIn[hit - kEscapeOut];
            } else if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
        out += '"';
    }

    bool encodeTable(int t, int depth) {
        if (depth >= kMaxDepth || !lua_checkstack(L, 8)) return fail("nesting too deep (cyclic table?)");

        if (classify(L, t) != Kind::Object) {
            const int n = lua_objlen(L, t);
            out += '[';
            for (int i = 1; i <= n; ++i) {
                if (i > 1) out += ',';
                newline(depth + 1);
                lua_rawgeti(L, t, i); // a hole in a tagged array becomes null
                const bool ok = encode(lua_gettop(L), depth + 1);
                lua_pop(L, 1);
                if (!ok) return false;
            }
            if (n > 0) newline(depth);
            out += ']';
            return true;
        }

        // Object: collect keys as text, sort for deterministic output, then look values up.
        std::vector<std::string> keys;
        lua_pushnil(L);
        while (lua_next(L, t)) {
            const int type = lua_type(L, -2);
            if (type == LUA_TSTRING) {
                std::size_t length = 0;
                const char* k = lua_tolstring(L, -2, &length);
                keys.emplace_back(k, length);
            } else if (type == LUA_TNUMBER) {
                char buf[40];
                if (!formatNumber(lua_tonumber(L, -2), buf)) return fail("cannot encode Infinity as a key");
                keys.emplace_back(buf);
            } else {
                return fail("object keys must be strings or numbers");
            }
            lua_pop(L, 1);
        }
        std::sort(keys.begin(), keys.end());

        out += '{';
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) {
                if (keys[i] == keys[i - 1]) return fail("duplicate object key (e.g. 1 and \"1\")");
                out += ',';
            }
            newline(depth + 1);
            appendString(keys[i].data(), keys[i].size());
            out += indent.empty() ? ":" : ": ";

            lua_pushlstring(L, keys[i].data(), keys[i].size());
            lua_rawget(L, t);
            if (lua_isnil(L, -1)) { // the key was a number
                lua_pop(L, 1);
                lua_pushnumber(L, std::strtod(keys[i].c_str(), nullptr));
                lua_rawget(L, t);
            }
            const bool ok = encode(lua_gettop(L), depth + 1);
            lua_pop(L, 1);
            if (!ok) return false;
        }
        if (!keys.empty()) newline(depth);
        out += '}';
        return true;
    }

    bool encode(int idx, int depth) { // idx must be an absolute index
        switch (lua_type(L, idx)) {
        case LUA_TNIL:
            out += "null";
            return true;
        case LUA_TBOOLEAN:
            out += lua_toboolean(L, idx) ? "true" : "false";
            return true;
        case LUA_TNUMBER: {
            char buf[40];
            if (!formatNumber(lua_tonumber(L, idx), buf)) return fail("cannot encode NaN or Infinity");
            out += buf;
            return true;
        }
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, idx, &length);
            appendString(text, length);
            return true;
        }
        case LUA_TTABLE:
            return encodeTable(idx, depth);
        case LUA_TLIGHTUSERDATA:
            if (isNull(L, idx)) {
                out += "null";
                return true;
            }
            [[fallthrough]];
        default:
            return fail("cannot encode a value of type", luaL_typename(L, idx));
        }
    }
};

// json.encode(value: any, options: { indent: (number | string)? }?): string
int jsonEncode(lua_State* L) {
    luaL_checkany(L, 1); // nil is allowed and encodes as null

    int spaces = 0;
    const char* tabs = nullptr; // points into the options table's string, which stays alive
    std::size_t tabsLength = 0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "indent");
        const int type = lua_type(L, -1);
        if (type == LUA_TNUMBER) {
            const double d = lua_tonumber(L, -1);
            luaL_argcheck(L, d >= 0 && d <= 16, 2, "indent must be between 0 and 16");
            spaces = static_cast<int>(d);
        } else if (type == LUA_TSTRING) {
            tabs = lua_tolstring(L, -1, &tabsLength);
            for (std::size_t i = 0; i < tabsLength; ++i) {
                luaL_argcheck(L, tabs[i] == ' ' || tabs[i] == '\t', 2, "indent string may only contain spaces and tabs");
            }
        } else if (type != LUA_TNIL) {
            luaL_argerror(L, 2, "indent must be a number or a string");
        }
        lua_pop(L, 1);
    }

    const char* message = nullptr;
    const char* detail = nullptr;
    {
        Encoder enc{L, tabs ? std::string(tabs, tabsLength) : std::string(static_cast<std::size_t>(spaces), ' ')};
        if (enc.encode(1, 0)) {
            lua_pushlstring(L, enc.out.data(), enc.out.size());
        } else {
            message = enc.error;
            detail = enc.detail;
        }
    }

    if (message) luaL_error(L, "json.encode: %s%s%s", message, detail ? " " : "", detail ? detail : "");
    return 1;
}

// Paths ////////////////////////////////////////

// Pushes the path as an array table of segments (strings / numbers). A table path is used
// as given after validation; a string path is split without allocating C++ objects.
void pathToTable(lua_State* L, int arg) {
    if (lua_type(L, arg) != LUA_TSTRING) {
        luaL_checktype(L, arg, LUA_TTABLE);
        const int n = lua_objlen(L, arg);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, arg, i);
            const int type = lua_type(L, -1);
            luaL_argcheck(L, type == LUA_TSTRING || type == LUA_TNUMBER, arg, "path segments must be strings or numbers");
            lua_pop(L, 1);
        }
        lua_pushvalue(L, arg);
        return;
    }

    std::size_t length = 0;
    const char* p = lua_tolstring(L, arg, &length);
    lua_createtable(L, 0, 0);
    int count = 0;
    std::size_t i = 0;
    while (i < length) {
        if (p[i] == '[') {
            double index = 0;
            std::size_t j = i + 1;
            for (; j < length && p[j] >= '0' && p[j] <= '9'; ++j) index = index * 10 + (p[j] - '0');
            luaL_argcheck(L, j > i + 1 && j < length && p[j] == ']', arg, "invalid path: expected [digits]");
            luaL_argcheck(L, index >= 1 && index <= 1e9, arg, "invalid path: index must be 1 or more");
            lua_pushnumber(L, index);
            i = j + 1;
        } else {
            std::size_t j = i;
            while (j < length && p[j] != '.' && p[j] != '[') ++j;
            luaL_argcheck(L, j > i, arg, "invalid path: empty key");
            lua_pushlstring(L, p + i, j - i);
            i = j;
        }
        lua_rawseti(L, -2, ++count);

        if (i < length && p[i] == '.') {
            ++i;
            luaL_argcheck(L, i < length, arg, "invalid path: trailing '.'");
        }
    }
}

// A numeric segment must name an existing array slot (or the next free one when appending).
void checkSlot(lua_State* L, int tbl, int seg, bool append, const char* fn) {
    if (lua_type(L, seg) != LUA_TNUMBER) return;
    const double idx = lua_tonumber(L, seg);
    const double max = lua_objlen(L, tbl) + (append ? 1 : 0);
    if (idx < 1 || idx > max || idx != std::floor(idx)) luaL_error(L, "json.%s: array index out of range", fn);
}

// The container is on top of the stack. Replaces it with its child at segment `i` of the
// path table at index `path`. Returns false (stack unchanged) when the child is missing or
// not a table; with `create`, missing children are made and a non-table child is an error.
bool descend(lua_State* L, int path, int i, bool create, const char* fn) {
    const int cur = lua_gettop(L);
    lua_rawgeti(L, path, i);          // cur+1: segment
    lua_pushvalue(L, cur + 1);
    lua_rawget(L, cur);               // cur+2: child
    if (lua_isnil(L, -1) && create) {
        lua_pop(L, 1);
        checkSlot(L, cur, cur + 1, true, fn);
        lua_createtable(L, 0, 0);
        lua_pushvalue(L, cur + 1);
        lua_pushvalue(L, cur + 2);
        lua_rawset(L, cur);           // container[segment] = new child
    }
    if (lua_type(L, -1) != LUA_TTABLE) {
        if (create) luaL_error(L, "json.%s: path segment %d is not a container", fn, i);
        lua_settop(L, cur);
        return false;
    }
    lua_replace(L, cur);
    lua_settop(L, cur);
    return true;
}

// json.get(value: any, path: string | { string | number }, default: any?): any
int jsonGet(lua_State* L) {
    lua_settop(L, 3); // argument 3 is nil when absent
    pathToTable(L, 2); // index 4
    const int n = lua_objlen(L, 4);

    lua_pushvalue(L, 1);
    for (int i = 1; i <= n; ++i) {
        if (lua_type(L, -1) != LUA_TTABLE) {
            lua_pushvalue(L, 3);
            return 1;
        }
        lua_rawgeti(L, 4, i);
        lua_rawget(L, -2);
        lua_remove(L, -2);
    }

    if (lua_isnil(L, -1)) lua_pushvalue(L, 3);
    return 1;
}

// json.set(root: table, path: string | { string | number }, value: any): table
int jsonSet(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 3);
    luaL_argcheck(L, !lua_isnil(L, 3), 3, "value must not be nil (use json.null, or json.delete)");
    lua_settop(L, 3);
    pathToTable(L, 2); // index 4
    const int n = lua_objlen(L, 4);
    luaL_argcheck(L, n > 0, 2, "path must not be empty");

    lua_pushvalue(L, 1);
    for (int i = 1; i < n; ++i) descend(L, 4, i, true, "set");

    const int cur = lua_gettop(L);
    lua_rawgeti(L, 4, n);
    checkSlot(L, cur, cur + 1, true, "set");
    lua_pushvalue(L, 3);
    lua_rawset(L, cur);

    lua_pushvalue(L, 1);
    return 1;
}

// json.delete(root: table, path: string | { string | number }): any
// Returns the removed value, or nothing when the path did not exist.
int jsonDelete(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    pathToTable(L, 2); // index 3
    const int n = lua_objlen(L, 3);
    luaL_argcheck(L, n > 0, 2, "path must not be empty");

    lua_pushvalue(L, 1);
    for (int i = 1; i < n; ++i) {
        if (!descend(L, 3, i, false, "delete")) return 0;
    }

    const int cur = lua_gettop(L);
    lua_rawgeti(L, 3, n);          // cur+1: segment
    lua_pushvalue(L, cur + 1);
    lua_rawget(L, cur);            // cur+2: removed value (stays on top)
    if (lua_isnil(L, -1)) return 0;

    const double idx = lua_type(L, cur + 1) == LUA_TNUMBER ? lua_tonumber(L, cur + 1) : 0;
    const int len = lua_objlen(L, cur);
    if (idx >= 1 && idx <= len && idx == std::floor(idx)) { // array slot: shift the tail down
        for (int j = static_cast<int>(idx); j < len; ++j) {
            lua_rawgeti(L, cur, j + 1);
            lua_rawseti(L, cur, j);
        }
        lua_pushnil(L);
        lua_rawseti(L, cur, len);
    } else {
        lua_pushvalue(L, cur + 1);
        lua_pushnil(L);
        lua_rawset(L, cur);
    }
    return 1;
}

// Pushes a deep copy of the value at absolute index `idx`. Metatables (the array/object
// tags) are carried over by reference.
void cloneValue(lua_State* L, int idx, int depth) {
    if (lua_type(L, idx) != LUA_TTABLE) {
        lua_pushvalue(L, idx);
        return;
    }
    if (depth > kMaxDepth) luaL_error(L, "json: nesting too deep (cyclic table?)");
    luaL_checkstack(L, 8, "json: nesting too deep");

    lua_createtable(L, lua_objlen(L, idx), 0);
    const int copy = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pushvalue(L, -2);                          // key copy
        cloneValue(L, lua_gettop(L) - 1, depth + 1);   // clone of the value below it
        lua_rawset(L, copy);
        lua_pop(L, 1);                                 // original value; key stays for next
    }
    if (lua_getmetatable(L, idx)) lua_setmetatable(L, copy);
}

// RFC 7396: object members merge recursively, json.null deletes, everything else replaces.
void mergeInto(lua_State* L, int target, int patch, int depth) {
    if (depth > kMaxDepth) luaL_error(L, "json.merge: nesting too deep (cyclic table?)");
    luaL_checkstack(L, 8, "json.merge: nesting too deep");

    lua_pushnil(L);
    while (lua_next(L, patch)) {
        const int key = lua_gettop(L) - 1;
        const int value = key + 1;
        if (isNull(L, value)) {
            lua_pushvalue(L, key);
            lua_pushnil(L);
            lua_rawset(L, target);
        } else if (lua_type(L, value) == LUA_TTABLE && classify(L, value) != Kind::Array) {
            lua_pushvalue(L, key);
            lua_rawget(L, target);
            if (lua_type(L, -1) != LUA_TTABLE || classify(L, lua_gettop(L)) == Kind::Array) {
                lua_pop(L, 1);
                lua_createtable(L, 0, 0);
                lua_pushvalue(L, key);
                lua_pushvalue(L, -2);
                lua_rawset(L, target);
            }
            mergeInto(L, lua_gettop(L), value, depth + 1);
            lua_pop(L, 1);
        } else {
            lua_pushvalue(L, key);
            cloneValue(L, value, depth + 1);
            lua_rawset(L, target);
        }
        lua_pop(L, 1); // value; key stays for next
    }
}

// json.merge(target: table, patch: table): table
int jsonMerge(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_argcheck(L, !lua_rawequal(L, 1, 2), 2, "patch must not be the target itself");
    luaL_argcheck(L, classify(L, 1) != Kind::Array, 1, "target must be an object");
    luaL_argcheck(L, classify(L, 2) != Kind::Array, 2, "patch must be an object");

    mergeInto(L, 1, 2, 0);
    lua_settop(L, 1);
    return 1;
}

// json.clone(value: any): any
int jsonClone(lua_State* L) {
    luaL_checkany(L, 1);
    cloneValue(L, 1, 0);
    return 1;
}

// json.array(t: table?): table  /  json.object(t: table?): table
// Tags `t` (or a new table) so it always encodes as [] / {}. Replaces any metatable it had.
int tagTable(lua_State* L, const char* registryKey) {
    if (lua_isnoneornil(L, 1)) {
        lua_createtable(L, 0, 0);
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 1);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, registryKey);
    lua_setmetatable(L, -2);
    return 1;
}

int jsonArray(lua_State* L) {
    return tagTable(L, kArrayMeta);
}

int jsonObject(lua_State* L) {
    return tagTable(L, kObjectMeta);
}

constexpr NativeFunction kFunctions[] = {
    {"decode", jsonDecode},
    {"encode", jsonEncode},
    {"get", jsonGet},
    {"set", jsonSet},
    {"delete", jsonDelete},
    {"merge", jsonMerge},
    {"clone", jsonClone},
    {"array", jsonArray},
    {"object", jsonObject},
};

} // namespace

void openJson(lua_State* L) {
    for (const char* key : {kArrayMeta, kObjectMeta}) {
        lua_createtable(L, 0, 0);
        lua_setreadonly(L, -1, true);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 2);
    setFunctions(L, kFunctions);

    pushNull(L);
    lua_setfield(L, -2, "null");
}

} // namespace sonata::lib::libs
