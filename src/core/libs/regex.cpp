// @sonata/regex: regular expressions (ECMAScript syntax, backed by std::regex).
//
//     local regex = require("@sonata/regex")
//
// Argument order is always (text, pattern, ...). "flags" is an optional
// string; supported flags: "i" (case-insensitive).
//
//     regex.test(text, pattern, flags?)            --> boolean    match anywhere
//     regex.matches(text, pattern, flags?)         --> boolean    whole text must match
//     regex.find(text, pattern, flags?, init?)     --> Match?     first match from init (1-based)
//     regex.findAll(text, pattern, flags?)         --> { Match }
//     regex.replace(text, pattern, repl, flags?, limit?) --> string, count
//                                                    repl supports $1, $&, $$ ; limit < 0 = all
//     regex.split(text, pattern, flags?)           --> { string }  (empty matches are ignored)
//     regex.filter(list, pattern, flags?, invert?) --> { string }  keep (or drop) matching strings
//     regex.escape(text)                           --> string     literal -> safe pattern
//     regex.isValid(pattern, flags?)               --> boolean, string?  (false + error message)
//     regex.patterns.email / integer / number / identifier / uuid / ipv4 / hex
//
//     Match = { text: string, start: number, stop: number, groups: { string | false } }
//     start/stop are 1-based and inclusive; groups that did not participate are `false`.
//
//     regex.matches("2024-05-01", "\\d{4}-\\d{2}-\\d{2}")   --> true
//     regex.replace("a1b22", "\\d+", "<$&>")               --> "a<1>b<22>", 2
//     regex.filter({ "a.lua", "b.txt" }, "\\.lua$")        --> { "a.lua" }
//
// Invalid patterns raise a Lua error. Inputs are size-limited (kMaxText,
// kMaxPattern) because std::regex recurses and can exhaust the native stack.

#include <cstdio>
#include <iterator>
#include <new>
#include <regex>
#include <string>
#include <string_view>

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"


namespace sonata::lib::libs {

namespace {

constexpr std::size_t kMaxText = 1 << 16;
constexpr std::size_t kMaxPattern = 1024;

using Flags = std::regex::flag_type;

// argument reading ////////////////////////////////////////////

struct Input {
    const char* text;
    std::size_t textLength;
    const char* pattern;
    std::size_t patternLength;
    Flags flags;

    const char* end() const { return text + textLength; }
};

const char* readPattern(lua_State* L, int index, std::size_t& length) {
    const char* pattern = luaL_checklstring(L, index, &length);
    luaL_argcheck(L, length <= kMaxPattern, index, "pattern too long");
    return pattern;
}

Flags readFlags(lua_State* L, int index) {
    Flags flags = std::regex::ECMAScript;
    for (const char* f = luaL_optstring(L, index, ""); *f; ++f) {
        luaL_argcheck(L, *f == 'i', index, "unknown flag (supported: i)");
        flags = flags | std::regex::icase;
    }
    return flags;
}

// (text, pattern, flags?) with text at 1, pattern at patternIndex.
Input readInput(lua_State* L, int patternIndex, int flagsIndex) {
    Input in{};
    in.text = luaL_checklstring(L, 1, &in.textLength);
    luaL_argcheck(L, in.textLength <= kMaxText, 1, "text too long");
    in.pattern = readPattern(L, patternIndex, in.patternLength);
    in.flags = readFlags(L, flagsIndex);
    return in;
}

// C++ side /////////////////////////////////

std::regex compile(const Input& in) {
    return std::regex(in.pattern, in.patternLength, in.flags);
}

// Runs "body" (which must not raise Lua errors) and converts C++ regex
// failures into a Lua error AFTER all of body's objects were destroyed.
template <class F>
int guarded(lua_State* L, F body) {
    char err[200] = {0};
    int results = 0;

    try {
        results = body();
    } catch (const std::regex_error& e) {
        std::snprintf(err, sizeof(err), "regex error: %s", e.what());
    } catch (const std::bad_alloc&) {
        std::snprintf(err, sizeof(err), "regex error: out of memory");
    }

    if (err[0] != '\0') {
        luaL_error(L, "%s", err);
    }
    return results;
}

void pushMatch(lua_State* L, const std::cmatch& m, const char* base) {
    lua_createtable(L, 0, 4);

    lua_pushlstring(L, m[0].first, static_cast<std::size_t>(m[0].length()));
    lua_setfield(L, -2, "text");
    lua_pushinteger(L, static_cast<int>(m[0].first - base) + 1);
    lua_setfield(L, -2, "start");
    lua_pushinteger(L, static_cast<int>(m[0].second - base));
    lua_setfield(L, -2, "stop");

    lua_createtable(L, static_cast<int>(m.size()) - 1, 0);
    for (std::size_t i = 1; i < m.size(); ++i) {
        if (m[i].matched) {
            lua_pushlstring(L, m[i].first, static_cast<std::size_t>(m[i].length()));
        } else {
            lua_pushboolean(L, 0);
        }
        lua_rawseti(L, -2, static_cast<int>(i));
    }
    lua_setfield(L, -2, "groups");
}

// bindings //////////////////////////////////////////////////

// regex.test(text, pattern, flags?): boolean
int regexTest(lua_State* L) {
    const Input in = readInput(L, 2, 3);
    return guarded(L, [&] {
        const std::regex re = compile(in);
        lua_pushboolean(L, std::regex_search(in.text, in.end(), re));
        return 1;
    });
}

// regex.matches(text, pattern, flags?): boolean (whole text)
int regexMatches(lua_State* L) {
    const Input in = readInput(L, 2, 3);
    return guarded(L, [&] {
        const std::regex re = compile(in);
        lua_pushboolean(L, std::regex_match(in.text, in.end(), re));
        return 1;
    });
}

// regex.find(text, pattern, flags?, init?): Match?
int regexFind(lua_State* L) {
    const Input in = readInput(L, 2, 3);
    const int init = luaL_optinteger(L, 4, 1);
    luaL_argcheck(L, init >= 1 && static_cast<std::size_t>(init) <= in.textLength + 1, 4, "init out of range");

    return guarded(L, [&] {
        const std::regex re = compile(in);
        std::cmatch m;
        const auto flags = init > 1 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;

        if (std::regex_search(in.text + init - 1, in.end(), m, re, flags)) {
            pushMatch(L, m, in.text);
        } else {
            lua_pushnil(L);
        }
        return 1;
    });
}

// regex.findAll(text, pattern, flags?): { Match }
int regexFindAll(lua_State* L) {
    const Input in = readInput(L, 2, 3);
    return guarded(L, [&] {
        const std::regex re = compile(in);
        lua_createtable(L, 0, 0);

        int n = 0;
        for (std::cregex_iterator it(in.text, in.end(), re), stop; it != stop; ++it) {
            pushMatch(L, *it, in.text);
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    });
}

// regex.replace(text, pattern, replacement, flags?, limit?): string, number
int regexReplace(lua_State* L) {
    const Input in = readInput(L, 2, 4);
    std::size_t replLength = 0;
    const char* repl = luaL_checklstring(L, 3, &replLength);
    const int limit = luaL_optinteger(L, 5, -1);

    return guarded(L, [&] {
        const std::regex re = compile(in);
        std::string out;
        int count = 0;
        const char* last = in.text;

        for (std::cregex_iterator it(in.text, in.end(), re), stop; it != stop && (limit < 0 || count < limit); ++it) {
            const std::cmatch& m = *it;
            out.append(last, m[0].first);
            m.format(std::back_inserter(out), repl, repl + replLength);
            last = m[0].second;
            ++count;
        }
        out.append(last, in.end());

        lua_pushlstring(L, out.data(), out.size());
        lua_pushinteger(L, count);
        return 2;
    });
}

// regex.split(text, pattern, flags?): { string }
int regexSplit(lua_State* L) {
    const Input in = readInput(L, 2, 3);
    return guarded(L, [&] {
        const std::regex re = compile(in);
        lua_createtable(L, 0, 0);

        int n = 0;
        const char* last = in.text;
        for (std::cregex_iterator it(in.text, in.end(), re), stop; it != stop; ++it) {
            const std::cmatch& m = *it;
            if (m.length(0) == 0) {
                continue;
            }
            lua_pushlstring(L, last, static_cast<std::size_t>(m[0].first - last));
            lua_rawseti(L, -2, ++n);
            last = m[0].second;
        }

        lua_pushlstring(L, last, static_cast<std::size_t>(in.end() - last));
        lua_rawseti(L, -2, ++n);
        return 1;
    });
}

// regex.filter(list: { string }, pattern, flags?, invert: boolean?): { string }
int regexFilter(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    Input in{};
    in.pattern = readPattern(L, 2, in.patternLength);
    in.flags = readFlags(L, 3);
    const bool invert = lua_toboolean(L, 4) != 0;
    const int size = lua_objlen(L, 1);

    for (int i = 1; i <= size; ++i) { // validate every element up front
        lua_rawgeti(L, 1, i);
        luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING && static_cast<std::size_t>(lua_objlen(L, -1)) <= kMaxText, 1,
                      "array of strings (each within the size limit) expected");
        lua_pop(L, 1);
    }

    return guarded(L, [&] {
        const std::regex re = compile(in);
        lua_createtable(L, 0, 0);

        int kept = 0;
        for (int i = 1; i <= size; ++i) {
            std::size_t length = 0;
            lua_rawgeti(L, 1, i);
            const char* s = lua_tolstring(L, -1, &length);

            if (std::regex_search(s, s + length, re) != invert) {
                lua_rawseti(L, -2, ++kept);
            } else {
                lua_pop(L, 1);
            }
        }
        return 1;
    });
}

// regex.escape(text: string): string
int regexEscape(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    constexpr std::string_view special = "\\^$.|?*+()[]{}";

    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
        if (special.find(text[i]) != std::string_view::npos) {
            out.push_back('\\');
        }
        out.push_back(text[i]);
    }

    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// regex.isValid(pattern, flags?): boolean, string?
int regexIsValid(lua_State* L) {
    std::size_t length = 0;
    const char* pattern = readPattern(L, 1, length);
    const Flags flags = readFlags(L, 2);

    char err[200] = {0};
    try {
        const std::regex re(pattern, length, flags);
    } catch (const std::regex_error& e) {
        std::snprintf(err, sizeof(err), "%s", e.what());
    }

    lua_pushboolean(L, err[0] == '\0');
    if (err[0] == '\0') {
        return 1;
    }
    lua_pushstring(L, err);
    return 2;
}

constexpr NativeFunction kFunctions[] = {
    {"test", regexTest},
    {"matches", regexMatches},
    {"find", regexFind},
    {"findAll", regexFindAll},
    {"replace", regexReplace},
    {"split", regexSplit},
    {"filter", regexFilter},
    {"escape", regexEscape},
    {"isValid", regexIsValid},
};

struct NamedPattern {
    const char* name;
    const char* pattern;
};

constexpr NamedPattern kPatterns[] = {
    {"email", R"re(^[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}$)re"},
    {"integer", R"re(^[+-]?[0-9]+$)re"},
    {"number", R"re(^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$)re"},
    {"identifier", R"re(^[A-Za-z_][A-Za-z0-9_]*$)re"},
    {"uuid", R"re(^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$)re"},
    {"ipv4", R"re(^((25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\.){3}(25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])$)re"},
    {"hex", R"re(^(0[xX])?[0-9a-fA-F]+$)re"},
};

} // namespace

void openRegex(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 2);
    setFunctions(L, kFunctions);

    lua_createtable(L, 0, static_cast<int>(std::size(kPatterns)));
    for (const NamedPattern& p : kPatterns) {
        lua_pushstring(L, p.pattern);
        lua_setfield(L, -2, p.name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "patterns");
}

} // namespace sonata::lib::libs