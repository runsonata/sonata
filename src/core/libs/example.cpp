// @sonata/example: a minimal template for a built-in library.
// !! This file is NOT loaded in runtime. !!
//
//     local example = require("@sonata/example")
//
//     example.repeatString("ab", 3, "-")   --> "ab-ab-ab"
//     example.splitOnce("k=v=w", "=")      --> "k", "v=w"
//     example.splitOnce("kv", "=")         --> "kv", nil
//     example.words("  a bb  c ")          --> { "a", "bb", "c" }
//     local next = example.newCounter(10)
//     next(), next()                       --> 10, 11
//
// To add a library like this:
//   1. Copy this file and rename openExample() / the functions.
//   2. Declare the opener in libs.hpp.
//   3. Register it under its require() name ("@sonata/example") wherever the
//      other built-ins (such as openPath) are registered.

#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"


namespace sonata::lib::libs {

namespace {

// Rule of thumb for every binding: read and validate ALL arguments first
// (luaL_check* / luaL_argcheck raise a Lua error), and only then create C++
// objects. An argument error then never skips a destructor, whether Luau was
// built with C++ exceptions or longjmp.

// example.repeatString(text: string, count: number, separator: string?): string
int exampleRepeatString(lua_State* L) {
    std::size_t textLength = 0;
    std::size_t sepLength = 0;
    const char* text = luaL_checklstring(L, 1, &textLength);
    const int count = luaL_checkinteger(L, 2);
    const char* sep = luaL_optlstring(L, 3, "", &sepLength); // optional argument

    luaL_argcheck(L, count >= 0 && count <= 10000, 2, "count must be between 0 and 10000");

    std::string result;
    for (int i = 0; i < count; ++i) {
        if (i != 0) {
            result.append(sep, sepLength);
        }
        result.append(text, textLength);
    }

    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// example.splitOnce(text: string, delimiter: string): (string, string?)
// Returning several values: push each one, return how many you pushed.
int exampleSplitOnce(lua_State* L) {
    std::size_t textLength = 0;
    std::size_t delimLength = 0;
    const char* text = luaL_checklstring(L, 1, &textLength);
    const char* delim = luaL_checklstring(L, 2, &delimLength);

    luaL_argcheck(L, delimLength != 0, 2, "delimiter must not be empty");

    const std::string_view view(text, textLength);
    const std::size_t at = view.find(std::string_view(delim, delimLength));

    if (at == std::string_view::npos) {
        lua_pushlstring(L, text, textLength);
        lua_pushnil(L);
        return 2;
    }

    lua_pushlstring(L, text, at);
    lua_pushlstring(L, text + at + delimLength, textLength - at - delimLength);
    return 2;
}

// example.words(text: string): { string }
// Building and returning a table (an array here).
int exampleWords(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);

    lua_createtable(L, 0, 0);

    int index = 0;
    std::size_t pos = 0;

    while (pos < length) {
        while (pos < length && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }

        const std::size_t start = pos;
        while (pos < length && !std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }

        if (pos > start) {
            lua_pushlstring(L, text + start, pos - start);
            lua_rawseti(L, -2, ++index);
        }
    }

    return 1;
}

// The function returned by example.newCounter(). Its state lives in upvalue 1,
// so every counter (and every VM) has its own, with no C++ globals involved.
int counterNext(lua_State* L) {
    const double current = lua_tonumber(L, lua_upvalueindex(1));

    lua_pushnumber(L, current + 1);
    lua_replace(L, lua_upvalueindex(1));

    lua_pushnumber(L, current);
    return 1;
}

// example.newCounter(start: number?): () -> number
int exampleNewCounter(lua_State* L) {
    const double start = luaL_optnumber(L, 1, 0);

    lua_pushnumber(L, start);
    lua_pushcclosure(L, counterNext, "counter", 1); // 1 upvalue, taken from the stack
    return 1;
}

constexpr NativeFunction kFunctions[] = {
    {"repeatString", exampleRepeatString},
    {"splitOnce", exampleSplitOnce},
    {"words", exampleWords},
    {"newCounter", exampleNewCounter},
};

} // namespace

// The opener: runs once per VM and must push exactly one value, which is
// what require("@sonata/example") returns. The loader makes the table
// read-only afterwards.
void openExample(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 1);
    setFunctions(L, kFunctions);

}

} // namespace sonata::lib::libs