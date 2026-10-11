// @sonata/net URLs (net.url) and internationalized domain names (net.idna).

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs::net {

namespace {

using OptView = std::optional<std::string_view>;

struct UrlParts {
    OptView scheme, username, password, host, query, fragment;
    std::string_view path;
    int port = -1;
};

// RFC 3986 split. Components stay raw (not decoded). Returns an error message or nullptr.
const char* parseUrl(std::string_view s, UrlParts& u) {
    constexpr auto npos = std::string_view::npos;

    for (const char c : s) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
            return "invalid character in url";
        }
    }

    if (const auto at = s.find('#'); at != npos) {
        u.fragment = s.substr(at + 1);
        s = s.substr(0, at);
    }
    if (const auto at = s.find('?'); at != npos) {
        u.query = s.substr(at + 1);
        s = s.substr(0, at);
    }

    if (!s.empty() && isAlpha(s[0])) {
        std::size_t i = 1;
        while (i < s.size() && (isAlpha(s[i]) || isDigit(s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) {
            ++i;
        }
        if (i < s.size() && s[i] == ':') {
            u.scheme = s.substr(0, i);
            s.remove_prefix(i + 1);
        }
    }

    if (s.substr(0, 2) == "//") {
        s.remove_prefix(2);
        const auto slash = s.find('/');
        std::string_view authority = s.substr(0, slash);
        s = slash == npos ? std::string_view() : s.substr(slash);

        if (const auto at = authority.rfind('@'); at != npos) {
            const std::string_view userinfo = authority.substr(0, at);
            authority.remove_prefix(at + 1);

            const auto colon = userinfo.find(':');
            u.username = userinfo.substr(0, colon);
            if (colon != npos) {
                u.password = userinfo.substr(colon + 1);
            }
        }

        std::string_view portText;
        if (!authority.empty() && authority.front() == '[') { // [IPv6]:port
            const auto close = authority.find(']');
            if (close == npos) {
                return "invalid host";
            }
            u.host = authority.substr(1, close - 1);

            const std::string_view rest = authority.substr(close + 1);
            if (!rest.empty()) {
                if (rest.front() != ':') {
                    return "invalid host";
                }
                portText = rest.substr(1);
            }
        } else {
            const auto colon = authority.rfind(':');
            if (colon != authority.find(':')) {
                return "invalid host"; // several colons without brackets
            }
            u.host = authority.substr(0, colon);
            if (colon != npos) {
                portText = authority.substr(colon + 1);
            }
        }

        if (!portText.empty()) {
            int port = 0;
            for (const char c : portText) {
                if (!isDigit(c) || (port = port * 10 + (c - '0')) > 65535) {
                    return "invalid port";
                }
            }
            u.port = port;
        }
    }

    u.path = s;
    return nullptr;
}

void buildUrl(const UrlParts& u, std::string& out) {
    if (u.scheme) {
        out += *u.scheme;
        out += ':';
    }
    if (u.host) {
        out += "//";
        if (u.username) {
            out += *u.username;
            if (u.password) {
                out += ':';
                out += *u.password;
            }
            out += '@';
        }

        const bool ipv6 = u.host->find(':') != std::string_view::npos;
        out += ipv6 ? "[" : "";
        out += *u.host;
        out += ipv6 ? "]" : "";

        if (u.port >= 0) {
            out += ':';
            out += std::to_string(u.port);
        }
        if (!u.path.empty() && u.path.front() != '/') {
            out += '/';
        }
    }
    out += u.path;
    if (u.query) {
        out += '?';
        out += *u.query;
    }
    if (u.fragment) {
        out += '#';
        out += *u.fragment;
    }
}

int defaultPortOf(std::string_view scheme) {
    const std::string lower = lowercase(scheme);
    if (lower == "http" || lower == "ws") return 80;
    if (lower == "https" || lower == "wss") return 443;
    if (lower == "ftp") return 21;
    if (lower == "ssh") return 22;
    if (lower == "smtp") return 25;
    if (lower == "gopher") return 70;
    if (lower == "socks5" || lower == "socks5h") return 1080;
    return -1;
}

// RFC 3986 5.2.4.
std::string removeDotSegments(std::string_view input) {
    std::string output;
    while (!input.empty()) {
        if (input.substr(0, 3) == "../") {
            input.remove_prefix(3);
        } else if (input.substr(0, 2) == "./") {
            input.remove_prefix(2);
        } else if (input.substr(0, 3) == "/./") {
            input.remove_prefix(2);
        } else if (input == "/.") {
            input = "/";
        } else if (input.substr(0, 4) == "/../" || input == "/..") {
            input = input.size() == 3 ? std::string_view("/") : input.substr(3);
            const std::size_t slash = output.rfind('/');
            output.erase(slash == std::string::npos ? 0 : slash);
        } else if (input == "." || input == "..") {
            input = {};
        } else {
            const std::size_t next = input.find('/', input[0] == '/' ? 1 : 0);
            output.append(input.substr(0, next));
            input = next == std::string_view::npos ? std::string_view() : input.substr(next);
        }
    }
    return output;
}

std::string mergePaths(const UrlParts& base, std::string_view ref) {
    if (base.host && base.path.empty()) {
        return "/" + std::string(ref);
    }
    const std::size_t slash = base.path.rfind('/');
    if (slash == std::string_view::npos) {
        return std::string(ref);
    }
    return std::string(base.path.substr(0, slash + 1)) + std::string(ref);
}

// Resolves "ref" against "base" (RFC 3986 5.2.2). Error message or nullptr.
const char* resolveUrl(std::string_view baseText, std::string_view refText, std::string& out) {
    UrlParts base;
    UrlParts ref;
    if (const char* err = parseUrl(baseText, base)) {
        return err;
    }
    if (const char* err = parseUrl(refText, ref)) {
        return err;
    }
    if (!base.scheme) {
        return "the base URL must be absolute";
    }

    UrlParts target;
    std::string path;
    if (ref.scheme) {
        target = ref;
        path = removeDotSegments(ref.path);
    } else {
        if (ref.host) {
            target = ref;
            path = removeDotSegments(ref.path);
        } else {
            target.username = base.username;
            target.password = base.password;
            target.host = base.host;
            target.port = base.port;
            if (ref.path.empty()) {
                path = std::string(base.path);
                target.query = ref.query ? ref.query : base.query;
            } else {
                path = ref.path.front() == '/' ? removeDotSegments(ref.path) : removeDotSegments(mergePaths(base, ref.path));
                target.query = ref.query;
            }
        }
        target.scheme = base.scheme;
    }
    target.fragment = ref.fragment;
    target.path = path;
    buildUrl(target, out);
    return nullptr;
}

// Upper-cases percent-escapes and decodes the ones that stand for unreserved characters.
std::string normalizeEscapes(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hexValue(text[i + 1]);
            const int lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                const char c = static_cast<char>(hi * 16 + lo);
                if (isAlpha(c) || isDigit(c) || c == '-' || c == '.' || c == '_' || c == '~') {
                    out += c;
                } else {
                    out += '%';
                    out += kHex[hi];
                    out += kHex[lo];
                }
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

void setField(lua_State* L, const char* name, const OptView& value, bool lower = false) {
    if (!value) {
        return;
    }
    if (lower) {
        setStringField(L, name, lowercase(*value));
    } else {
        setStringField(L, name, *value);
    }
}

// Reads a string, number or boolean at "idx" as text (numbers are converted in place on the stack).
const char* scalarText(lua_State* L, int idx, std::size_t* len) {
    if (lua_type(L, idx) == LUA_TBOOLEAN) {
        const bool value = lua_toboolean(L, idx) != 0;
        *len = value ? 4 : 5;
        return value ? "true" : "false";
    }
    return lua_isstring(L, idx) ? lua_tolstring(L, idx, len) : nullptr;
}

void addPair(std::string& out, std::string_view key, std::string_view value) {
    if (!out.empty()) {
        out += '&';
    }
    percentEncode(out, key, {});
    out += '=';
    percentEncode(out, value, {});
}

// Appends the pairs of the table at absolute index "t": first the array part ({key, value} entries),
// then the string keys (sorted, so the result does not depend on table order). Error message or nullptr.
const char* appendQuery(lua_State* L, int t, std::string& out) {
    const int n = static_cast<int>(lua_objlen(L, t));

    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, t, i);
        if (!lua_istable(L, -1)) {
            return "query list entries must be {key, value} tables";
        }
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);

        std::size_t keyLength = 0;
        std::size_t valueLength = 0;
        const char* key = scalarText(L, -2, &keyLength);
        const char* value = scalarText(L, -1, &valueLength);
        if (key == nullptr || value == nullptr) {
            return "query keys and values must be strings, numbers or booleans";
        }
        addPair(out, std::string_view(key, keyLength), std::string_view(value, valueLength));
        lua_pop(L, 3);
    }

    std::vector<std::pair<std::string, std::string>> named;
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            std::size_t keyLength = 0;
            std::size_t valueLength = 0;
            const char* key = lua_tolstring(L, -2, &keyLength);
            if (lua_istable(L, -1)) { // key = { "a", "b" }: the key repeated
                const int count = static_cast<int>(lua_objlen(L, -1));
                for (int i = 1; i <= count; ++i) {
                    lua_rawgeti(L, -1, i);
                    const char* value = scalarText(L, -1, &valueLength);
                    if (value == nullptr) {
                        lua_pop(L, 3);
                        return "query values must be strings, numbers or booleans";
                    }
                    named.emplace_back(std::string(key, keyLength), std::string(value, valueLength));
                    lua_pop(L, 1);
                }
            } else {
                const char* value = scalarText(L, -1, &valueLength);
                if (value == nullptr) {
                    lua_pop(L, 2);
                    return "query values must be strings, numbers or booleans";
                }
                named.emplace_back(std::string(key, keyLength), std::string(value, valueLength));
            }
        }
        lua_pop(L, 1);
    }
    std::stable_sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [key, value] : named) {
        addPair(out, key, value);
    }
    return nullptr;
}

// Optional string field of the table at index 1. The value is left on the stack on purpose (it keeps the string alive).
OptView stringField(lua_State* L, const char* name) {
    return optStringField(L, 1, name, "url.build");
}

// net.url.parse(url: string): { scheme, username, password, host, port, path, query, fragment } | (nil, string)
int urlParse(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);

    UrlParts u;
    if (const char* error = parseUrl(std::string_view(text, len), u)) {
        return failure(L, error);
    }

    lua_createtable(L, 0, 8);
    setField(L, "scheme", u.scheme, true);
    setField(L, "username", u.username);
    setField(L, "password", u.password);
    setField(L, "host", u.host, true);
    if (u.port >= 0) {
        lua_pushinteger(L, u.port);
        lua_setfield(L, -2, "port");
    }
    setField(L, "path", u.path);
    setField(L, "query", u.query);
    setField(L, "fragment", u.fragment);
    return 1;
}

// net.url.build(parts: { scheme, username, password, host, port, path, query: string | table, fragment }): string
// username/password/port are only used together with host.
int urlBuild(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    UrlParts u;
    u.scheme = stringField(L, "scheme");
    u.username = stringField(L, "username");
    u.password = stringField(L, "password");
    u.host = stringField(L, "host");
    u.fragment = stringField(L, "fragment");
    u.path = stringField(L, "path").value_or("");

    lua_getfield(L, 1, "port");
    if (!lua_isnil(L, -1)) {
        const double port = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
        if (!(port >= 0 && port <= 65535) || port != std::floor(port)) {
            luaL_error(L, "url.build: invalid port");
        }
        u.port = static_cast<int>(port);
    }

    lua_getfield(L, 1, "query");
    const int queryIndex = lua_gettop(L);
    const bool queryIsTable = lua_istable(L, queryIndex);
    if (!lua_isnil(L, queryIndex) && !queryIsTable) {
        u.query = stringField(L, "query"); // raises unless it is a string or number
    }

    const char* err = nullptr;
    {
        std::string query;
        std::string out;
        if (queryIsTable) {
            err = appendQuery(L, queryIndex, query);
            u.query = query;
        }
        if (err == nullptr) {
            buildUrl(u, out);
            pushString(L, out);
            return 1;
        }
    }
    luaL_error(L, "url.build: %s", err);
    return 0;
}

// net.url.encode(text: string, safe: string?): string
int urlEncode(lua_State* L) {
    std::size_t len = 0;
    std::size_t safeLength = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    const char* safe = luaL_optlstring(L, 2, "", &safeLength);

    std::string out;
    percentEncode(out, std::string_view(text, len), std::string_view(safe, safeLength));
    pushString(L, out);
    return 1;
}

// net.url.decode(text: string, plusAsSpace: boolean?): string | (nil, string)
int urlDecode(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    const bool plus = lua_toboolean(L, 2) != 0;

    std::string out;
    if (!percentDecode(out, std::string_view(text, len), plus)) {
        return failure(L, "invalid percent-escape");
    }
    pushString(L, out);
    return 1;
}

// net.url.parseQuery(query: string): ({ [string]: string }, { { string } }) | (nil, string)
// The map keeps the first value of a repeated key; the list keeps every pair in order.
int urlParseQuery(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);

    std::string_view query(text, len);
    if (!query.empty() && query.front() == '?') {
        query.remove_prefix(1);
    }

    lua_createtable(L, 0, 0); // 2: map
    lua_createtable(L, 0, 0); // 3: list
    int count = 0;

    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view item = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        if (item.empty()) {
            continue;
        }

        const auto eq = item.find('=');
        std::string key;
        std::string value;
        if (!percentDecode(key, item.substr(0, eq), true) ||
            (eq != std::string_view::npos && !percentDecode(value, item.substr(eq + 1), true))) {
            return failure(L, "invalid percent-escape");
        }

        pushString(L, key);
        lua_pushvalue(L, -1);
        lua_rawget(L, 2);
        const bool seen = !lua_isnil(L, -1);
        lua_pop(L, 1); // stack: key

        pushString(L, value); // key, value

        lua_createtable(L, 2, 0);
        lua_pushvalue(L, -3);
        lua_rawseti(L, -2, 1);
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, 3, ++count);

        if (seen) {
            lua_pop(L, 2);
        } else {
            lua_rawset(L, 2);
        }
    }
    return 2;
}

// net.url.buildQuery(pairs: { { string } } | { [string]: string | number | boolean | { string } }): string
int urlBuildQuery(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    const char* err = nullptr;
    {
        std::string out;
        err = appendQuery(L, 1, out);
        if (err == nullptr) {
            pushString(L, out);
            return 1;
        }
    }
    luaL_error(L, "url.buildQuery: %s", err);
    return 0;
}

// net.url.resolve(base: string, reference: string): string | (nil, string)
int urlResolve(lua_State* L) {
    std::size_t baseLength = 0;
    std::size_t refLength = 0;
    const char* base = luaL_checklstring(L, 1, &baseLength);
    const char* ref = luaL_checklstring(L, 2, &refLength);

    std::string out;
    if (const char* err = resolveUrl(std::string_view(base, baseLength), std::string_view(ref, refLength), out)) {
        return failure(L, err);
    }
    pushString(L, out);
    return 1;
}

// net.url.normalize(url: string): string | (nil, string)
int urlNormalize(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);

    UrlParts u;
    if (const char* error = parseUrl(std::string_view(text, len), u)) {
        return failure(L, error);
    }
    const std::string scheme = u.scheme ? lowercase(*u.scheme) : std::string();
    const std::string host = u.host ? lowercase(*u.host) : std::string();
    std::string path = normalizeEscapes(removeDotSegments(u.path));
    if (u.host && path.empty()) {
        path = "/";
    }
    const std::string query = u.query ? normalizeEscapes(*u.query) : std::string();

    if (u.scheme) {
        u.scheme = scheme;
    }
    if (u.host) {
        u.host = host;
    }
    if (u.port >= 0 && u.scheme && u.port == defaultPortOf(*u.scheme)) {
        u.port = -1;
    }
    u.path = path;
    if (u.query) {
        u.query = query;
    }

    std::string out;
    buildUrl(u, out);
    pushString(L, out);
    return 1;
}

// net.url.origin(url: string): string | (nil, string)   "https://example.com:8443"
int urlOrigin(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);

    UrlParts u;
    if (const char* error = parseUrl(std::string_view(text, len), u)) {
        return failure(L, error);
    }
    if (!u.scheme || !u.host) {
        return failure(L, "the URL has no scheme or host");
    }
    UrlParts origin;
    const std::string scheme = lowercase(*u.scheme);
    const std::string host = lowercase(*u.host);
    origin.scheme = scheme;
    origin.host = host;
    origin.port = u.port == defaultPortOf(scheme) ? -1 : u.port;

    std::string out;
    buildUrl(origin, out);
    pushString(L, out);
    return 1;
}

// net.url.defaultPort(scheme: string): number?
int urlDefaultPort(lua_State* L) {
    const int port = defaultPortOf(luaL_checkstring(L, 1));
    if (port < 0) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, port);
    }
    return 1;
}

// IDNA / punycode (RFC 3492) /////////////////////////////////////////////////

constexpr int kBase = 36, kTMin = 1, kTMax = 26, kSkew = 38, kDamp = 700, kInitialBias = 72, kInitialN = 128;

int adapt(int delta, int points, bool first) {
    delta = first ? delta / kDamp : delta / 2;
    delta += delta / points;
    int k = 0;
    while (delta > ((kBase - kTMin) * kTMax) / 2) {
        delta /= kBase - kTMin;
        k += kBase;
    }
    return k + (kBase - kTMin + 1) * delta / (delta + kSkew);
}

char encodeDigit(int d) {
    return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26));
}

bool decodeUtf8(std::string_view text, std::vector<std::uint32_t>& out) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) {
            return false;
        }
        std::uint32_t code = length == 1 ? lead : lead & (0xFF >> (length + 1));
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        out.push_back(code);
        i += length;
    }
    return true;
}

void appendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

bool punyEncode(const std::vector<std::uint32_t>& input, std::string& out) {
    std::uint32_t n = kInitialN;
    int delta = 0;
    int bias = kInitialBias;
    std::size_t handled = 0;
    for (const std::uint32_t c : input) {
        if (c < 0x80) {
            out += toLower(static_cast<char>(c));
            ++handled;
        }
    }
    const std::size_t basic = handled;
    if (basic > 0) {
        out += '-';
    }
    while (handled < input.size()) {
        std::uint32_t m = 0x10FFFF;
        for (const std::uint32_t c : input) {
            if (c >= n && c < m) {
                m = c;
            }
        }
        if ((m - n) > static_cast<std::uint32_t>((0x7FFFFFFF - delta) / static_cast<int>(handled + 1))) {
            return false;
        }
        delta += static_cast<int>(m - n) * static_cast<int>(handled + 1);
        n = m;
        for (const std::uint32_t c : input) {
            if (c < n && ++delta == 0) {
                return false;
            }
            if (c == n) {
                int q = delta;
                for (int k = kBase;; k += kBase) {
                    const int t = k <= bias ? kTMin : k >= bias + kTMax ? kTMax : k - bias;
                    if (q < t) {
                        break;
                    }
                    out += encodeDigit(t + (q - t) % (kBase - t));
                    q = (q - t) / (kBase - t);
                }
                out += encodeDigit(q);
                bias = adapt(delta, static_cast<int>(handled + 1), handled == basic);
                delta = 0;
                ++handled;
            }
        }
        ++delta;
        ++n;
    }
    return true;
}

bool punyDecode(std::string_view input, std::vector<std::uint32_t>& out) {
    std::uint32_t n = kInitialN;
    int i = 0;
    int bias = kInitialBias;
    const std::size_t dash = input.rfind('-');
    if (dash != std::string_view::npos) {
        for (std::size_t j = 0; j < dash; ++j) {
            if (static_cast<unsigned char>(input[j]) >= 0x80) {
                return false;
            }
            out.push_back(static_cast<unsigned char>(input[j]));
        }
    }
    for (std::size_t in = dash == std::string_view::npos ? 0 : dash + 1; in < input.size();) {
        const int oldi = i;
        int w = 1;
        for (int k = kBase;; k += kBase) {
            if (in >= input.size()) {
                return false;
            }
            const char c = input[in++];
            const int digit = isDigit(c) ? c - '0' + 26 : isAlpha(c) ? toLower(c) - 'a' : -1;
            if (digit < 0 || digit > (0x7FFFFFFF - i) / w) {
                return false;
            }
            i += digit * w;
            const int t = k <= bias ? kTMin : k >= bias + kTMax ? kTMax : k - bias;
            if (digit < t) {
                break;
            }
            w *= kBase - t;
        }
        const int count = static_cast<int>(out.size()) + 1;
        bias = adapt(i - oldi, count, oldi == 0);
        n += static_cast<std::uint32_t>(i / count);
        i %= count;
        out.insert(out.begin() + i, n);
        ++i;
    }
    return true;
}

std::vector<std::string_view> splitLabels(std::string_view domain) {
    std::vector<std::string_view> labels;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = domain.find('.', start);
        labels.push_back(domain.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos) {
            return labels;
        }
        start = dot + 1;
    }
}

// net.idna.toAscii(domain: string): string | (nil, string)
int idnaToAscii(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);

    std::string out;
    bool first = true;
    for (const std::string_view label : splitLabels(std::string_view(text, len))) {
        if (!first) {
            out += '.';
        }
        first = false;
        std::vector<std::uint32_t> codes;
        if (!decodeUtf8(label, codes)) {
            return failure(L, "invalid UTF-8");
        }
        const bool ascii = std::all_of(codes.begin(), codes.end(), [](std::uint32_t c) { return c < 0x80; });
        if (ascii) {
            out += lowercase(label);
            continue;
        }
        std::string encoded;
        if (!punyEncode(codes, encoded)) {
            return failure(L, "label too long");
        }
        out += "xn--" + encoded;
    }
    pushString(L, out);
    return 1;
}

// net.idna.toUnicode(domain: string): string | (nil, string)
int idnaToUnicode(lua_State* L) {
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);

    std::string out;
    bool first = true;
    for (const std::string_view label : splitLabels(std::string_view(text, len))) {
        if (!first) {
            out += '.';
        }
        first = false;
        if (label.size() > 4 && lowercase(label.substr(0, 4)) == "xn--") {
            std::vector<std::uint32_t> codes;
            if (!punyDecode(label.substr(4), codes)) {
                return failure(L, "invalid punycode");
            }
            for (const std::uint32_t c : codes) {
                appendUtf8(out, c);
            }
        } else {
            out += label;
        }
    }
    pushString(L, out);
    return 1;
}

const Function kUrlFunctions[] = {
    {"parse", urlParse},
    {"build", urlBuild},
    {"encode", urlEncode},
    {"decode", urlDecode},
    {"parseQuery", urlParseQuery},
    {"buildQuery", urlBuildQuery},
    {"resolve", urlResolve},
    {"normalize", urlNormalize},
    {"origin", urlOrigin},
    {"defaultPort", urlDefaultPort},
};

const Function kIdnaFunctions[] = {
    {"toAscii", idnaToAscii},
    {"toUnicode", idnaToUnicode},
};

} // namespace

void pushUrl(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kUrlFunctions)));
    setFunctionList(L, kUrlFunctions);
}

void pushIdna(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kIdnaFunctions)));
    setFunctionList(L, kIdnaFunctions);
}

} // namespace sonata::lib::libs::net
