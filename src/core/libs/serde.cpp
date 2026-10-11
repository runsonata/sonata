// @sonata/serde: parse, serialize and convert structured text, plus a toolkit for your own formats.
//
//     local serde = require("@sonata/serde")
//
//     serde.decode("yaml", text, opts?)         --> any       built-ins: yaml (yml), toml, ini, csv, tsv
//     serde.encode("toml", value, opts?)        --> string
//     serde.convert("yaml", "toml", text, opts?) --> string   decode + encode in one call
//     serde.null                                              stands for null (Lua tables cannot hold nil)
//     serde.formats()                           --> { string }
//
//   opts (all optional; unknown keys are ignored, and the same table is handed to custom formats):
//     indent: number      yaml indentation, 2..8 (default 2)
//     sort: boolean       encode: sort table keys (default true; false = Lua's iteration order)
//     dropNull: boolean   decode: leave out null table entries (array nulls stay serde.null)
//     header: boolean     csv: first row is a header (default true -> array of tables)
//     typed: boolean      ini (default true) / csv (default false): turn numbers and booleans into values
//     delimiter: string   csv: one character (tsv defaults to "\t")
//
//   Your own formats. Wherever a format name is accepted, a table works too:
//
//     serde.register("kv", {
//         decode = function(text: string, opts: table?): any ... end,
//         encode = function(value: any, opts: table?): string ... end,
//     })                                       -- either function may be omitted
//     serde.register("kv", nil)                -- unregister (registered names win over built-ins)
//     serde.decode({ decode = function(text) ... end }, text)   -- inline, no registration
//
//   Toolkit for writing them:
//     local s = serde.scanner(text)            fields: s.text, s.pos (0-based; assign to backtrack)
//     s:eof(), s:peek(n?), s:next(n?), s:eat(lit) -> bool, s:expect(lit), s:line(), s:rest(),
//     s:take(set)   consume while chars are in set: literal chars, ranges "a-z", classes %s %d %a %w %x,
//                   leading "^" negates ("^,\n" = everything up to a comma or newline)
//     s:where() --> line, column               s:fail(msg) raises "msg (line L, column C)"
//     local w = serde.writer(indent: string | number | nil)     -- all methods return w
//     w:write(...), w:line(...), w:indent(n?), w:dedent(n?), w:str()   (tostring(w) works too)
//     serde.escape(text, quote?), serde.unescape(text), serde.formatNumber(n), serde.isArray(t)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <new>
#include <string>
#include <string_view>

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "serde_formats.hpp"

namespace sonata::lib::libs {

namespace {

using serde::Fail;
using serde::Fmt;
using serde::Node;
using serde::Opts;

char kNullTag; // the address of this variable is serde.null
constexpr const char* kFormatsKey = "@sonata/serde:formats";
constexpr const char* kScannerKey = "@sonata/serde:scanner";
constexpr const char* kWriterKey = "@sonata/serde:writer";

struct Builtin {
    const char* name;
    Fmt fmt;
    char delim;
};
constexpr Builtin kBuiltins[] = {
    {"yaml", Fmt::Yaml, ','}, {"yml", Fmt::Yaml, ','}, {"toml", Fmt::Toml, ','},
    {"ini", Fmt::Ini, ','},   {"csv", Fmt::Csv, ','},  {"tsv", Fmt::Csv, '\t'},
};

template <class F>
bool guarded(lua_State* L, F&& fn) {
    try {
        fn();
        return true;
    } catch (const Fail& f) {
        lua_pushlstring(L, f.msg.data(), f.msg.size());
    } catch (const std::bad_alloc&) {
        lua_pushstring(L, "out of memory");
    }
    return false;
}

[[noreturn]] void raiseFail(lua_State* L, const char* what) {
    luaL_error(L, "serde.%s: %s", what, lua_tostring(L, -1));
}

// Lua < - > Node ////////////////////////////////////////

// Sequence = every key is an integer in 1..count (empty tables count as sequences).
bool isSeq(lua_State* L, int idx, std::size_t& count) {
    count = 0;
    double max = 0;
    bool ok = true;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        ++count;
        if (lua_type(L, -2) != LUA_TNUMBER) {
            ok = false;
        } else {
            const double d = lua_tonumber(L, -2);
            if (d >= 1 && d == std::floor(d)) max = std::max(max, d);
            else ok = false;
        }
        lua_pop(L, 1);
    }
    return ok && max <= static_cast<double>(count);
}

void sortMap(Node& n) {
    std::vector<std::size_t> ix(n.keys.size());
    for (std::size_t i = 0; i < ix.size(); ++i) ix[i] = i;
    std::sort(ix.begin(), ix.end(), [&](std::size_t a, std::size_t b) { return n.keys[a] < n.keys[b]; });
    std::vector<std::string> k;
    std::vector<Node> v;
    for (const std::size_t i : ix) {
        k.push_back(std::move(n.keys[i]));
        v.push_back(std::move(n.kids[i]));
    }
    n.keys = std::move(k);
    n.kids = std::move(v);
}

// "idx" must be an absolute stack index. Raw access only, so no Lua code can run in here.
Node toNode(lua_State* L, int idx, int depth, bool sort) {
    switch (lua_type(L, idx)) {
    case LUA_TNIL: return {};
    case LUA_TBOOLEAN: return Node::boolean(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER: return Node::number(lua_tonumber(L, idx));
    case LUA_TSTRING: {
        std::size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        return Node::string(std::string(s, n));
    }
    case LUA_TLIGHTUSERDATA:
        if (lua_tolightuserdata(L, idx) == &kNullTag) return {};
        break;
    case LUA_TTABLE: {
        if (depth > 200) throw Fail{"nesting too deep (cyclic table?)"};
        if (!lua_checkstack(L, 6)) throw Fail{"stack overflow"};
        std::size_t count = 0;
        if (isSeq(L, idx, count)) {
            Node out = Node::array();
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(L, idx, static_cast<int>(i));
                out.kids.push_back(toNode(L, lua_gettop(L), depth + 1, sort));
                lua_pop(L, 1);
            }
            return out;
        }
        Node out = Node::map();
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            std::string key;
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::size_t n = 0;
                const char* s = lua_tolstring(L, -2, &n);
                key.assign(s, n);
            } else if (lua_type(L, -2) == LUA_TNUMBER) {
                key = serde::fmtNum(lua_tonumber(L, -2));
            } else {
                throw Fail{std::string("cannot use a ") + luaL_typename(L, -2) + " as a key"};
            }
            out.keys.push_back(std::move(key));
            out.kids.push_back(toNode(L, lua_gettop(L), depth + 1, sort));
            lua_pop(L, 1);
        }
        if (sort) sortMap(out);
        return out;
    }
    default: break;
    }
    throw Fail{std::string("cannot serialize a ") + luaL_typename(L, idx)};
}

void pushNode(lua_State* L, const Node& n, bool dropNull) {
    if (!lua_checkstack(L, 4)) throw Fail{"stack overflow"};
    switch (n.type) {
    case Node::Null: lua_pushlightuserdata(L, &kNullTag); break;
    case Node::Boolean: lua_pushboolean(L, n.b); break;
    case Node::Number: lua_pushnumber(L, n.n); break;
    case Node::String: lua_pushlstring(L, n.s.data(), n.s.size()); break;
    case Node::Array:
        lua_createtable(L, static_cast<int>(n.kids.size()), 0);
        for (std::size_t i = 0; i < n.kids.size(); ++i) {
            pushNode(L, n.kids[i], dropNull);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        break;
    case Node::Map:
        lua_createtable(L, 0, static_cast<int>(n.kids.size()));
        for (std::size_t i = 0; i < n.kids.size(); ++i) {
            if (dropNull && n.kids[i].type == Node::Null) continue;
            lua_pushlstring(L, n.keys[i].data(), n.keys[i].size());
            pushNode(L, n.kids[i], dropNull);
            lua_rawset(L, -3);
        }
        break;
    }
}

// options and format lookup ////////////////////////////////////////////////

// Leaves the field's value on the stack and returns true when it is present (type-checked).
bool optField(lua_State* L, int idx, const char* key, int type) {
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (lua_type(L, -1) != type) luaL_error(L, "options.%s must be a %s", key, lua_typename(L, type));
    return true;
}

void readOpts(lua_State* L, int idx, Opts& o) {
    if (lua_isnoneornil(L, idx)) return;
    luaL_argcheck(L, lua_type(L, idx) == LUA_TTABLE, idx, "options must be a table");
    if (optField(L, idx, "indent", LUA_TNUMBER)) {
        o.indent = std::clamp(static_cast<int>(lua_tonumber(L, -1)), 2, 8);
        lua_pop(L, 1);
    }
    if (optField(L, idx, "sort", LUA_TBOOLEAN)) { o.sort = lua_toboolean(L, -1) != 0; lua_pop(L, 1); }
    if (optField(L, idx, "dropNull", LUA_TBOOLEAN)) { o.dropNull = lua_toboolean(L, -1) != 0; lua_pop(L, 1); }
    if (optField(L, idx, "header", LUA_TBOOLEAN)) { o.header = lua_toboolean(L, -1) != 0; lua_pop(L, 1); }
    if (optField(L, idx, "typed", LUA_TBOOLEAN)) { o.typed = lua_toboolean(L, -1) ? 1 : 0; lua_pop(L, 1); }
    if (optField(L, idx, "delimiter", LUA_TSTRING)) {
        std::size_t n = 0;
        const char* d = lua_tolstring(L, -1, &n);
        if (n != 1) luaL_error(L, "options.delimiter must be a single character");
        o.delim = d[0];
        lua_pop(L, 1);
    }
}

// Custom format -> pushes its table and returns true. Built-in -> fills fmt/delim and returns false.
bool resolve(lua_State* L, int idx, Fmt& fmt, char& delim) {
    if (lua_type(L, idx) == LUA_TTABLE) {
        lua_pushvalue(L, idx);
        return true;
    }
    const char* name = luaL_checklstring(L, idx, nullptr);
    lua_getfield(L, LUA_REGISTRYINDEX, kFormatsKey);
    lua_getfield(L, -1, name);
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_remove(L, -2);
        return true;
    }
    lua_pop(L, 2);
    for (const Builtin& b : kBuiltins) {
        if (std::strcmp(b.name, name) == 0) {
            fmt = b.fmt;
            delim = b.delim;
            return false;
        }
    }
    luaL_error(L, "unknown format '%s'", name);
}

// Stack: [..., format]. Calls format[field](a, b) and leaves only the result in its place.
void callCustom(lua_State* L, const char* field, int a, int b) {
    lua_getfield(L, -1, field);
    if (lua_type(L, -1) != LUA_TFUNCTION) luaL_error(L, "format has no '%s' function", field);
    lua_pushvalue(L, a);
    lua_pushvalue(L, b);
    lua_call(L, 2, 1);
    lua_remove(L, -2);
}

// All indices are absolute; each leaves exactly one value on top of the stack.
void runDecode(lua_State* L, int fmtIdx, int textIdx, int optsIdx) {
    Fmt fmt = Fmt::Yaml;
    Opts o;
    if (resolve(L, fmtIdx, fmt, o.delim)) {
        callCustom(L, "decode", textIdx, optsIdx);
        return;
    }
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, textIdx, &len);
    readOpts(L, optsIdx, o);
    if (!guarded(L, [&] { pushNode(L, serde::decodeText(fmt, std::string_view(text, len), o), o.dropNull); }))
        raiseFail(L, "decode");
}

void runEncode(lua_State* L, int fmtIdx, int valIdx, int optsIdx) {
    Fmt fmt = Fmt::Yaml;
    Opts o;
    if (resolve(L, fmtIdx, fmt, o.delim)) {
        callCustom(L, "encode", valIdx, optsIdx);
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "custom encode must return a string");
        return;
    }
    readOpts(L, optsIdx, o);
    if (!guarded(L, [&] {
            const std::string out = serde::encodeNode(fmt, toNode(L, valIdx, 0, o.sort), o);
            lua_pushlstring(L, out.data(), out.size());
        }))
        raiseFail(L, "encode");
}

// serde.decode(format: string | table, text: string, options: table?): any
int serdeDecode(lua_State* L) {
    lua_settop(L, 3);
    runDecode(L, 1, 2, 3);
    return 1;
}

// serde.encode(format: string | table, value: any, options: table?): string
int serdeEncode(lua_State* L) {
    lua_settop(L, 3);
    runEncode(L, 1, 2, 3);
    return 1;
}

// serde.convert(from, to, text: string, options: table?): string
int serdeConvert(lua_State* L) {
    lua_settop(L, 4);
    runDecode(L, 1, 3, 4);
    runEncode(L, 2, 5, 4);
    return 1;
}

// serde.register(name: string, format: { decode: function?, encode: function? }?)
int serdeRegister(lua_State* L) {
    const char* name = luaL_checklstring(L, 1, nullptr);
    lua_settop(L, 2);
    if (!lua_isnil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        bool any = false;
        for (const char* field : {"decode", "encode"}) {
            lua_getfield(L, 2, field);
            const int t = lua_type(L, -1);
            if (t != LUA_TNIL && t != LUA_TFUNCTION) luaL_error(L, "format.%s must be a function", field);
            any = any || t == LUA_TFUNCTION;
            lua_pop(L, 1);
        }
        luaL_argcheck(L, any, 2, "format needs a decode and/or encode function");
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kFormatsKey);
    lua_pushvalue(L, 2); // nil removes the entry
    lua_setfield(L, -2, name);
    return 0;
}

// serde.formats(): { string }
int serdeFormats(lua_State* L) {
    lua_createtable(L, 8, 0);
    const int out = lua_gettop(L);
    int n = 0;
    for (const Builtin& b : kBuiltins) {
        lua_pushstring(L, b.name);
        lua_rawseti(L, out, ++n);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kFormatsKey);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_rawseti(L, out, ++n);
    }
    lua_settop(L, out);
    return 1;
}

// small string tools /////////////////////////////////////

// serde.escape(text: string, quote: string?): string
int serdeEscape(lua_State* L) {
    std::size_t n = 0, ql = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const char* q = luaL_optlstring(L, 2, "\"", &ql);
    luaL_argcheck(L, ql == 1, 2, "quote must be a single character");
    std::string out;
    serde::escapeInto(out, std::string_view(s, n), q[0]);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// serde.unescape(text: string): string   (\n \t \r \b \f \e \0 \" \' \\ \/ \xNN \uNNNN \UNNNNNNNN)
int serdeUnescape(lua_State* L) {
    std::size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    if (!guarded(L, [&] {
            const std::string_view t(s, n);
            std::string out;
            for (std::size_t i = 0; i < n;) {
                if (t[i] != '\\') {
                    out += t[i++];
                    continue;
                }
                ++i;
                if (!serde::unescapeOne(t, i, out)) throw Fail{"invalid escape sequence"};
            }
            lua_pushlstring(L, out.data(), out.size());
        }))
        raiseFail(L, "unescape");
    return 1;
}

// serde.formatNumber(n: number): string   shortest text that reads back as the same number
int serdeFormatNumber(lua_State* L) {
    const std::string s = serde::numText(luaL_checknumber(L, 1), false);
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

// serde.isArray(t: table): boolean   true for {} and for tables whose keys are exactly 1..n
int serdeIsArray(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::size_t count = 0;
    lua_pushboolean(L, isSeq(L, 1, count));
    return 1;
}

// scanner ///////////////////////////////////////////////////////

struct Sc {
    const char* s;
    std::size_t n;
    std::size_t i;
};

Sc scGet(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    Sc sc{nullptr, 0, 0};
    lua_rawgetfield(L, 1, "text");
    sc.s = lua_tolstring(L, -1, &sc.n); // stays alive: the table still holds the string
    lua_pop(L, 1);
    if (!sc.s) luaL_error(L, "scanner has no text");
    lua_rawgetfield(L, 1, "pos");
    const double p = lua_tonumber(L, -1);
    lua_pop(L, 1);
    sc.i = p >= static_cast<double>(sc.n) ? sc.n : (p > 0 ? static_cast<std::size_t>(p) : 0);
    return sc;
}

void scSet(lua_State* L, std::size_t i) {
    lua_pushnumber(L, static_cast<double>(i));
    lua_rawsetfield(L, 1, "pos");
}

void scWhere(const Sc& s, int& line, int& col) {
    line = 1;
    col = 1;
    for (std::size_t k = 0; k < s.i; ++k) {
        if (s.s[k] == '\n') { ++line; col = 1; }
        else ++col;
    }
}

bool inSet(const char* set, std::size_t n, unsigned char c) {
    const bool neg = n > 1 && set[0] == '^';
    bool hit = false;
    for (std::size_t i = neg ? 1 : 0; i < n && !hit; ++i) {
        const unsigned char a = static_cast<unsigned char>(set[i]);
        if (a == '%' && i + 1 < n) {
            const unsigned char k = static_cast<unsigned char>(set[++i]);
            switch (k) {
            case 's': hit = std::isspace(c) != 0; break;
            case 'd': hit = std::isdigit(c) != 0; break;
            case 'a': hit = std::isalpha(c) != 0; break;
            case 'w': hit = std::isalnum(c) != 0; break;
            case 'x': hit = std::isxdigit(c) != 0; break;
            default: hit = c == k;
            }
        } else if (i + 2 < n && set[i + 1] == '-') {
            hit = c >= a && c <= static_cast<unsigned char>(set[i + 2]);
            i += 2;
        } else {
            hit = c == a;
        }
    }
    return hit != neg;
}

// serde.scanner(text: string): Scanner
int serdeScanner(lua_State* L) {
    luaL_checklstring(L, 1, nullptr);
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, 1);
    lua_rawsetfield(L, -2, "text");
    lua_pushnumber(L, 0);
    lua_rawsetfield(L, -2, "pos");
    lua_getfield(L, LUA_REGISTRYINDEX, kScannerKey);
    lua_setmetatable(L, -2);
    return 1;
}

int scEof(lua_State* L) {
    const Sc s = scGet(L);
    lua_pushboolean(L, s.i >= s.n);
    return 1;
}

int scRead(lua_State* L, bool advance) {
    const Sc s = scGet(L);
    const int k = luaL_optinteger(L, 2, 1);
    luaL_argcheck(L, k >= 0, 2, "count must not be negative");
    const std::size_t m = std::min<std::size_t>(static_cast<std::size_t>(k), s.n - s.i);
    lua_pushlstring(L, s.s + s.i, m);
    if (advance) scSet(L, s.i + m);
    return 1;
}
int scPeek(lua_State* L) { return scRead(L, false); } // s:peek(n: number?): string
int scNext(lua_State* L) { return scRead(L, true); }  // s:next(n: number?): string

bool scMatch(lua_State* L, const Sc& s, const char*& lit, std::size_t& ln) {
    lit = luaL_checklstring(L, 2, &ln);
    return s.n - s.i >= ln && std::memcmp(s.s + s.i, lit, ln) == 0;
}

// s:eat(literal: string): boolean   consumes the literal if it is next
int scEat(lua_State* L) {
    const Sc s = scGet(L);
    const char* lit = nullptr;
    std::size_t ln = 0;
    const bool ok = scMatch(L, s, lit, ln);
    if (ok) scSet(L, s.i + ln);
    lua_pushboolean(L, ok);
    return 1;
}

// s:expect(literal: string): string   like eat, but raises with the position when it is not next
int scExpect(lua_State* L) {
    const Sc s = scGet(L);
    const char* lit = nullptr;
    std::size_t ln = 0;
    if (!scMatch(L, s, lit, ln)) {
        int line, col;
        scWhere(s, line, col);
        luaL_error(L, "expected '%s' (line %d, column %d)", lit, line, col);
    }
    scSet(L, s.i + ln);
    lua_pushlstring(L, lit, ln);
    return 1;
}

// s:take(set: string): string   consumes while characters are in the set
int scTake(lua_State* L) {
    const Sc s = scGet(L);
    std::size_t sl = 0;
    const char* set = luaL_checklstring(L, 2, &sl);
    std::size_t j = s.i;
    while (j < s.n && inSet(set, sl, static_cast<unsigned char>(s.s[j]))) ++j;
    lua_pushlstring(L, s.s + s.i, j - s.i);
    scSet(L, j);
    return 1;
}

// s:line(): string   the rest of the current line (without its newline); moves to the next line
int scLine(lua_State* L) {
    const Sc s = scGet(L);
    std::size_t j = s.i;
    while (j < s.n && s.s[j] != '\n') ++j;
    std::size_t end = j;
    if (end > s.i && s.s[end - 1] == '\r') --end;
    lua_pushlstring(L, s.s + s.i, end - s.i);
    scSet(L, j < s.n ? j + 1 : j);
    return 1;
}

// s:rest(): string   everything not consumed yet (does not move)
int scRest(lua_State* L) {
    const Sc s = scGet(L);
    lua_pushlstring(L, s.s + s.i, s.n - s.i);
    return 1;
}

// s:where(): (number, number)   line and column of the current position, both 1-based
int scWhereM(lua_State* L) {
    const Sc s = scGet(L);
    int line, col;
    scWhere(s, line, col);
    lua_pushnumber(L, line);
    lua_pushnumber(L, col);
    return 2;
}

// s:fail(message: string)   raises "message (line L, column C)"
int scFailM(lua_State* L) {
    const Sc s = scGet(L);
    const char* msg = luaL_checklstring(L, 2, nullptr);
    int line, col;
    scWhere(s, line, col);
    luaL_error(L, "%s (line %d, column %d)", msg, line, col);
}

//  writer ////////////////////////////////////

// serde.writer(indent: string | number | nil): Writer
int serdeWriter(lua_State* L) {
    lua_createtable(L, 0, 3);
    lua_createtable(L, 16, 0);
    lua_rawsetfield(L, -2, "parts");
    if (lua_type(L, 1) == LUA_TSTRING) {
        lua_pushvalue(L, 1);
    } else {
        char sp[17];
        const int n = lua_type(L, 1) == LUA_TNUMBER ? std::clamp(static_cast<int>(lua_tonumber(L, 1)), 0, 16) : 2;
        std::memset(sp, ' ', sizeof sp);
        lua_pushlstring(L, sp, static_cast<std::size_t>(n));
    }
    lua_rawsetfield(L, -2, "unit");
    lua_pushnumber(L, 0);
    lua_rawsetfield(L, -2, "level");
    lua_getfield(L, LUA_REGISTRYINDEX, kWriterKey);
    lua_setmetatable(L, -2);
    return 1;
}

void wAppend(lua_State* L, const char* s, std::size_t n) { // appends to self.parts (self is index 1)
    lua_rawgetfield(L, 1, "parts");
    lua_pushlstring(L, s, n);
    lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
    lua_pop(L, 1);
}

void wAppendArgs(lua_State* L, int from, int to) {
    for (int i = from; i <= to; ++i) {
        std::size_t n = 0;
        const char* s = luaL_tolstring(L, i, &n);
        wAppend(L, s, n);
        lua_pop(L, 1);
    }
}

// w:write(...): Writer   appends strings/numbers/booleans as they are
int wWrite(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    wAppendArgs(L, 2, lua_gettop(L));
    lua_pushvalue(L, 1);
    return 1;
}

// w:line(...): Writer   current indentation, then the arguments, then a newline (no args = blank line)
int wLine(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const int top = lua_gettop(L);
    if (top > 1) {
        lua_rawgetfield(L, 1, "level");
        const int level = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgetfield(L, 1, "unit");
        std::size_t un = 0;
        const char* unit = lua_tolstring(L, -1, &un);
        for (int k = 0; unit && k < level; ++k) wAppend(L, unit, un);
        lua_pop(L, 1);
        wAppendArgs(L, 2, top);
    }
    wAppend(L, "\n", 1);
    lua_pushvalue(L, 1);
    return 1;
}

int wShift(lua_State* L, int sign) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const int n = luaL_optinteger(L, 2, 1);
    lua_rawgetfield(L, 1, "level");
    const int level = static_cast<int>(lua_tonumber(L, -1)) + sign * n;
    lua_pop(L, 1);
    lua_pushnumber(L, level < 0 ? 0 : level);
    lua_rawsetfield(L, 1, "level");
    lua_pushvalue(L, 1);
    return 1;
}
int wIndent(lua_State* L) { return wShift(L, 1); }  // w:indent(n: number?): Writer
int wDedent(lua_State* L) { return wShift(L, -1); } // w:dedent(n: number?): Writer

// w:str(): string   everything written so far
int wStr(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string out;
    lua_rawgetfield(L, 1, "parts");
    for (int i = 1, n = lua_objlen(L, -1); i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        std::size_t l = 0;
        if (const char* s = lua_tolstring(L, -1, &l)) out.append(s, l);
        lua_pop(L, 1);
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

constexpr NativeFunction kFunctions[] = {
    {"decode", serdeDecode},       {"encode", serdeEncode},     {"convert", serdeConvert},
    {"register", serdeRegister},   {"formats", serdeFormats},   {"scanner", serdeScanner},
    {"writer", serdeWriter},       {"escape", serdeEscape},     {"unescape", serdeUnescape},
    {"formatNumber", serdeFormatNumber}, {"isArray", serdeIsArray},
};

constexpr NativeFunction kScannerFns[] = {
    {"eof", scEof},   {"peek", scPeek}, {"next", scNext}, {"eat", scEat},       {"expect", scExpect},
    {"take", scTake}, {"line", scLine}, {"rest", scRest}, {"where", scWhereM},  {"fail", scFailM},
};

constexpr NativeFunction kWriterFns[] = {
    {"write", wWrite}, {"line", wLine}, {"indent", wIndent}, {"dedent", wDedent}, {"str", wStr},
};

template <std::size_t N>
void makeMeta(lua_State* L, const char* registryKey, const NativeFunction (&fns)[N], const char* tostringFn) {
    lua_createtable(L, 0, 2);
    lua_createtable(L, 0, static_cast<int>(N));
    setFunctions(L, fns);
    if (tostringFn) {
        lua_getfield(L, -1, tostringFn);
        lua_setfield(L, -3, "__tostring");
    }
    lua_setfield(L, -2, "__index");
    lua_setfield(L, LUA_REGISTRYINDEX, registryKey);
}

} // namespace

void openSerde(lua_State* L) {
    lua_createtable(L, 0, 0);
    lua_setfield(L, LUA_REGISTRYINDEX, kFormatsKey);
    makeMeta(L, kScannerKey, kScannerFns, nullptr);
    makeMeta(L, kWriterKey, kWriterFns, "str");

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 2);
    setFunctions(L, kFunctions);

    lua_pushlightuserdata(L, &kNullTag);
    lua_setfield(L, -2, "null");
}

} // namespace sonata::lib::libs