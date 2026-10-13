// @sonata/net HTTP/1.1 building blocks (net.http) and media types (net.mime).
//
// No I/O happens here: these turn bytes into tables and tables into bytes, so an
// HTTP client or server can be written on top of the sockets in plain Luau.

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs::net {

namespace {

using Header = std::pair<std::string, std::string>;

constexpr std::size_t kDefaultMaxHead = 64 * 1024;
constexpr std::size_t kDefaultMaxHeaders = 100;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

bool validFieldValue(std::string_view value) {
    for (const char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

// Status texts ///////////////////////////////////////////////////////////////

const char* statusText(int code) {
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default: return nullptr;
    }
}

// Message heads ///////////////////////////////////////////////////////////

struct Head {
    std::string method, target, version, reason;
    int status = 0;
    std::vector<Header> headers;
};

enum class Parse { Done, More, Error };

// Next line of "data" from "pos" (without its "\n" or "\r\n"). False if there is no full line yet.
bool nextLine(std::string_view data, std::size_t& pos, std::string_view& line) {
    const std::size_t nl = data.find('\n', pos);
    if (nl == std::string_view::npos) {
        return false;
    }
    line = data.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    pos = nl + 1;
    return true;
}

bool parseVersion(std::string_view text, std::string& version) {
    if (text.size() != 8 || text.substr(0, 5) != "HTTP/" || !isDigit(text[5]) || text[6] != '.' || !isDigit(text[7])) {
        return false;
    }
    version = std::string(text.substr(5));
    return true;
}

Parse parseHead(std::string_view data, bool response, std::size_t maxSize, std::size_t maxHeaders, Head& head,
                std::size_t& consumed, std::string& err) {
    std::size_t pos = 0;
    if (!response) { // RFC 9112 2.2: ignore empty lines before a request
        while (pos < data.size() && (data[pos] == '\r' || data[pos] == '\n')) {
            ++pos;
        }
    }

    const auto incomplete = [&]() {
        if (data.size() > maxSize) {
            err = "message head too large";
            return Parse::Error;
        }
        return Parse::More;
    };

    std::string_view line;
    if (!nextLine(data, pos, line)) {
        return incomplete();
    }

    if (response) {
        // HTTP/1.1 200 OK
        const std::size_t space = line.find(' ');
        if (space == std::string_view::npos || !parseVersion(line.substr(0, space), head.version)) {
            err = "invalid status line";
            return Parse::Error;
        }
        const std::string_view rest = line.substr(space + 1);
        if (rest.size() < 3 || !isDigit(rest[0]) || !isDigit(rest[1]) || !isDigit(rest[2]) || (rest.size() > 3 && rest[3] != ' ')) {
            err = "invalid status code";
            return Parse::Error;
        }
        head.status = (rest[0] - '0') * 100 + (rest[1] - '0') * 10 + (rest[2] - '0');
        head.reason = rest.size() > 4 ? std::string(rest.substr(4)) : std::string();
    } else {
        // GET /path HTTP/1.1
        const std::size_t first = line.find(' ');
        const std::size_t second = first == std::string_view::npos ? first : line.find(' ', first + 1);
        if (second == std::string_view::npos || !parseVersion(line.substr(second + 1), head.version)) {
            err = "invalid request line";
            return Parse::Error;
        }
        head.method = std::string(line.substr(0, first));
        head.target = std::string(line.substr(first + 1, second - first - 1));
        if (!isToken(head.method) || head.target.empty() || head.target.find_first_of(" \t") != std::string::npos) {
            err = "invalid request line";
            return Parse::Error;
        }
    }

    for (;;) {
        if (!nextLine(data, pos, line)) {
            return incomplete();
        }
        if (line.empty()) {
            break;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            err = "obsolete line folding in a header";
            return Parse::Error;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || !isToken(line.substr(0, colon))) {
            err = "invalid header line";
            return Parse::Error;
        }
        const std::string_view value = trim(line.substr(colon + 1));
        if (value.find('\0') != std::string_view::npos || value.find('\r') != std::string_view::npos) {
            err = "invalid character in a header value";
            return Parse::Error;
        }
        if (head.headers.size() >= maxHeaders) {
            err = "too many headers";
            return Parse::Error;
        }
        head.headers.emplace_back(std::string(line.substr(0, colon)), std::string(value));
    }

    if (pos > maxSize) {
        err = "message head too large";
        return Parse::Error;
    }
    consumed = pos;
    return Parse::Done;
}

// Pushes the map (lower-case names; repeated headers joined with ", ", Set-Cookie with "\n")
// and the list ({ name, value } in order, names as received).
void pushHeaders(lua_State* L, const std::vector<Header>& headers) {
    lua_createtable(L, 0, static_cast<int>(headers.size()));
    for (const auto& [name, value] : headers) {
        const std::string key = lowercase(name);
        lua_getfield(L, -1, key.c_str());
        if (lua_isstring(L, -1)) {
            std::string joined = lua_tostring(L, -1);
            joined += key == "set-cookie" ? "\n" : ", ";
            joined += value;
            lua_pop(L, 1);
            setStringField(L, key.c_str(), joined);
        } else {
            lua_pop(L, 1);
            setStringField(L, key.c_str(), value);
        }
    }
    lua_setfield(L, -2, "headers");

    lua_createtable(L, static_cast<int>(headers.size()), 0);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        lua_createtable(L, 2, 0);
        pushString(L, headers[i].first);
        lua_rawseti(L, -2, 1);
        pushString(L, headers[i].second);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "headerList");
}

void readLimits(lua_State* L, int idx, std::size_t& maxHead, std::size_t& maxHeaders) {
    maxHead = kDefaultMaxHead;
    maxHeaders = kDefaultMaxHeaders;
    if (lua_isnoneornil(L, idx)) {
        return;
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "maxHeadSize");
    if (!lua_isnil(L, -1)) {
        maxHead = static_cast<std::size_t>(std::max(1.0, luaL_checknumber(L, -1)));
    }
    lua_getfield(L, idx, "maxHeaders");
    if (!lua_isnil(L, -1)) {
        maxHeaders = static_cast<std::size_t>(std::max(1.0, luaL_checknumber(L, -1)));
    }
    lua_pop(L, 2);
}

int parseMessage(lua_State* L, bool response) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    std::size_t maxHead = 0;
    std::size_t maxHeaders = 0;
    readLimits(L, 2, maxHead, maxHeaders);

    Head head;
    std::size_t consumed = 0;
    std::string err;
    switch (parseHead(std::string_view(data, length), response, maxHead, maxHeaders, head, consumed, err)) {
    case Parse::More:
        lua_pushnil(L);
        return 1;
    case Parse::Error:
        return failure(L, err);
    case Parse::Done:
        break;
    }

    lua_createtable(L, 0, 8);
    setStringField(L, "version", head.version);
    if (response) {
        setNumberField(L, "status", head.status);
        setStringField(L, "reason", head.reason);
    } else {
        setStringField(L, "method", head.method);
        setStringField(L, "target", head.target);
        const std::size_t question = head.target.find('?');
        setStringField(L, "path", std::string_view(head.target).substr(0, question));
        if (question != std::string::npos) {
            setStringField(L, "query", std::string_view(head.target).substr(question + 1));
        }
    }
    pushHeaders(L, head.headers);
    lua_pushnumber(L, static_cast<double>(consumed));
    return 2;
}

// net.http.parseRequest(data: string, options: { maxHeadSize, maxHeaders }?): (Request, number) | nil | (nil, string)
int httpParseRequest(lua_State* L) {
    return parseMessage(L, false);
}

// net.http.parseResponse(data: string, options?): (Response, number) | nil | (nil, string)
int httpParseResponse(lua_State* L) {
    return parseMessage(L, true);
}

// Collects the headers of a head table's "headers" field: a { name = value | { value } }
// map (sorted by name) or a { { name, value } } list. Raises on invalid names or values.
std::vector<Header> readHeaders(lua_State* L, int head, const char* where) {
    std::vector<Header> headers;
    lua_getfield(L, head, "headers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return headers;
    }
    if (!lua_istable(L, -1)) {
        luaL_error(L, "%s: 'headers' must be a table", where);
    }
    const int t = lua_gettop(L);
    const auto add = [&](const char* name, int valueIndex) {
        if (!isToken(name)) {
            luaL_error(L, "%s: invalid header name '%s'", where, name);
        }
        if (!lua_isstring(L, valueIndex) || lua_type(L, valueIndex) == LUA_TBOOLEAN) {
            luaL_error(L, "%s: the value of header '%s' must be a string or number", where, name);
        }
        std::size_t length = 0;
        const char* value = lua_tolstring(L, valueIndex, &length);
        if (!validFieldValue(std::string_view(value, length))) {
            luaL_error(L, "%s: the value of header '%s' contains a line break", where, name);
        }
        headers.emplace_back(name, std::string(value, length));
    };

    const int n = static_cast<int>(lua_objlen(L, t));
    if (n > 0) {
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, t, i);
            if (!lua_istable(L, -1)) {
                luaL_error(L, "%s: header list entries must be { name, value }", where);
            }
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            if (lua_type(L, -2) != LUA_TSTRING) {
                luaL_error(L, "%s: header names must be strings", where);
            }
            add(lua_tostring(L, -2), lua_gettop(L));
            lua_pop(L, 3);
        }
    } else {
        std::vector<std::string> names;
        lua_pushnil(L);
        while (lua_next(L, t) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                luaL_error(L, "%s: header names must be strings", where);
            }
            names.emplace_back(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            lua_getfield(L, t, name.c_str());
            if (lua_istable(L, -1)) {
                const int count = static_cast<int>(lua_objlen(L, -1));
                for (int i = 1; i <= count; ++i) {
                    lua_rawgeti(L, -1, i);
                    add(name.c_str(), lua_gettop(L));
                    lua_pop(L, 1);
                }
            } else {
                add(name.c_str(), lua_gettop(L));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return headers;
}

void appendHeaders(std::string& out, const std::vector<Header>& headers) {
    for (const auto& [name, value] : headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "\r\n";
}

std::string checkVersion(lua_State* L, const char* where) {
    const auto version = optStringField(L, 1, "version", where);
    const std::string text = version ? std::string(*version) : "1.1";
    std::string ignored;
    if (!parseVersion("HTTP/" + text, ignored)) {
        luaL_error(L, "%s: invalid version '%s'", where, text.c_str());
    }
    return text;
}

// net.http.formatRequest(head: { method, target, version?, headers? }): string
int httpFormatRequest(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const std::string method(optStringField(L, 1, "method", "formatRequest").value_or("GET"));
    const std::string target(optStringField(L, 1, "target", "formatRequest").value_or("/"));
    if (!isToken(method)) {
        luaL_error(L, "formatRequest: invalid method '%s'", method.c_str());
    }
    if (target.empty() || target.find_first_of(" \t\r\n") != std::string::npos || target.find('\0') != std::string::npos) {
        luaL_error(L, "formatRequest: invalid target");
    }
    const std::string version = checkVersion(L, "formatRequest");
    const std::vector<Header> headers = readHeaders(L, 1, "formatRequest");

    std::string out = method + " " + target + " HTTP/" + version + "\r\n";
    appendHeaders(out, headers);
    pushString(L, out);
    return 1;
}

// net.http.formatResponse(head: { status, reason?, version?, headers? }): string
int httpFormatResponse(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "status");
    const int status = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    if (status < 100 || status > 999) {
        luaL_error(L, "formatResponse: 'status' must be a number from 100 to 999");
    }
    const auto reasonField = optStringField(L, 1, "reason", "formatResponse");
    const char* known = statusText(status);
    const std::string reason = reasonField ? std::string(*reasonField) : (known != nullptr ? known : "");
    if (!validFieldValue(reason)) {
        luaL_error(L, "formatResponse: invalid reason");
    }
    const std::string version = checkVersion(L, "formatResponse");
    const std::vector<Header> headers = readHeaders(L, 1, "formatResponse");

    std::string out = "HTTP/" + version + " " + std::to_string(status) + " " + reason + "\r\n";
    appendHeaders(out, headers);
    pushString(L, out);
    return 1;
}

// net.http.statusText(code: number): string?
int httpStatusText(lua_State* L) {
    const char* text = statusText(static_cast<int>(luaL_checknumber(L, 1)));
    if (text == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, text);
    }
    return 1;
}

// Every value of a header in a head table: from headerList if present, else from the
// headers map (looked up without regard to case). Empty if absent.
std::vector<std::string> headerValues(lua_State* L, int head, const char* name) {
    std::vector<std::string> values;
    lua_getfield(L, head, "headerList");
    if (lua_istable(L, -1)) {
        const int n = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                if (lua_isstring(L, -2) && lua_isstring(L, -1) && lowercase(lua_tostring(L, -2)) == name) {
                    values.emplace_back(lua_tostring(L, -1));
                }
                lua_pop(L, 2);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return values;
    }
    lua_pop(L, 1);

    lua_getfield(L, head, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lowercase(lua_tostring(L, -2)) == name) {
                if (lua_isstring(L, -1)) {
                    values.emplace_back(lua_tostring(L, -1));
                } else if (lua_istable(L, -1)) {
                    const int n = static_cast<int>(lua_objlen(L, -1));
                    for (int i = 1; i <= n; ++i) {
                        lua_rawgeti(L, -1, i);
                        if (lua_isstring(L, -1)) {
                            values.emplace_back(lua_tostring(L, -1));
                        }
                        lua_pop(L, 1);
                    }
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return values;
}

// Splits a comma-separated header list (commas inside quoted strings do not count).
std::vector<std::string> splitList(std::string_view value) {
    std::vector<std::string> items;
    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (quoted && c == '\\' && i + 1 < value.size()) {
            current += c;
            current += value[++i];
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
        }
        if (c == ',' && !quoted) {
            const std::string_view item = trim(current);
            if (!item.empty()) {
                items.emplace_back(item);
            }
            current.clear();
            continue;
        }
        current += c;
    }
    const std::string_view item = trim(current);
    if (!item.empty()) {
        items.emplace_back(item);
    }
    return items;
}

std::vector<std::string> tokensOf(lua_State* L, int head, const char* name) {
    std::vector<std::string> tokens;
    for (const std::string& value : headerValues(L, head, name)) {
        for (const std::string& item : splitList(value)) {
            tokens.push_back(lowercase(item));
        }
    }
    return tokens;
}

// net.http.bodyLength(head, options: { requestMethod: string? }?):
//     "none" | ("length", number) | "chunked" | "close" | "tunnel" | (nil, string)
int httpBodyLength(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string requestMethod;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        requestMethod = std::string(optStringField(L, 2, "requestMethod", "bodyLength").value_or(""));
    }
    lua_getfield(L, 1, "status");
    const bool response = !lua_isnil(L, -1);
    const int status = response ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);

    {
        if (response) {
            if (requestMethod == "HEAD" || (status >= 100 && status < 200) || status == 204 || status == 304) {
                lua_pushstring(L, "none");
                return 1;
            }
            if (requestMethod == "CONNECT" && status >= 200 && status < 300) {
                lua_pushstring(L, "tunnel");
                return 1;
            }
        }

        const std::vector<std::string> codings = tokensOf(L, 1, "transfer-encoding");
        if (!codings.empty()) {
            if (codings.back() == "chunked") {
                lua_pushstring(L, "chunked");
                return 1;
            }
            if (response) {
                lua_pushstring(L, "close");
                return 1;
            }
            return failure(L, "the request's transfer-encoding does not end with chunked");
        }

        const std::vector<std::string> lengths = headerValues(L, 1, "content-length");
        if (!lengths.empty()) {
            double length = -1;
            for (const std::string& value : lengths) {
                for (const std::string& item : splitList(value)) {
                    if (item.empty() || item.size() > 15 || !std::all_of(item.begin(), item.end(), isDigit)) {
                        return failure(L, "invalid content-length");
                    }
                    const double n = std::strtod(item.c_str(), nullptr);
                    if (length >= 0 && n != length) {
                        return failure(L, "conflicting content-length values");
                    }
                    length = n;
                }
            }
            if (length < 0) {
                return failure(L, "invalid content-length");
            }
            lua_pushstring(L, "length");
            lua_pushnumber(L, length);
            return 2;
        }
    }

    lua_pushstring(L, response ? "close" : "none");
    return 1;
}

// net.http.keepAlive(head): boolean   whether the connection stays open after this message
int httpKeepAlive(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const std::string version(optStringField(L, 1, "version", "keepAlive").value_or("1.1"));
    bool keep = version != "1.0" && version != "0.9";
    for (const std::string& token : tokensOf(L, 1, "connection")) {
        if (token == "close") {
            keep = false;
            break;
        }
        if (token == "keep-alive") {
            keep = true;
        }
    }
    lua_pushboolean(L, keep);
    return 1;
}

// Chunked transfer coding ////////////////////////////////////////////////////

// net.http.encodeChunk(data: string): string   ("" for empty data: an empty chunk would end the body)
int httpEncodeChunk(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    if (length == 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    char size[32];
    std::snprintf(size, sizeof size, "%zx\r\n", length);
    std::string out = size;
    out.append(data, length);
    out += "\r\n";
    pushString(L, out);
    return 1;
}

// net.http.lastChunk(trailers: Headers?): string
int httpLastChunk(lua_State* L) {
    std::string out = "0\r\n";
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_createtable(L, 0, 1);
        lua_pushvalue(L, 1);
        lua_setfield(L, -2, "headers");
        const std::vector<Header> trailers = readHeaders(L, lua_gettop(L), "lastChunk");
        appendHeaders(out, trailers);
    } else {
        out += "\r\n";
    }
    pushString(L, out);
    return 1;
}

constexpr const char* kDecoderMeta = "sonata.net.ChunkedDecoder";

struct ChunkedDecoder {
    enum class State { Size, Data, DataEnd, Trailers, Done, Failed } state = State::Size;
    std::uint64_t remaining = 0;
    std::string buffer;
    std::vector<Header> trailers;
    std::size_t trailerBytes = 0;
    std::size_t maxTrailerSize = 16 * 1024;
};

ChunkedDecoder* checkDecoder(lua_State* L) {
    return static_cast<ChunkedDecoder*>(luaL_checkudata(L, 1, kDecoderMeta));
}

// Advances the decoder over its buffer. Appends body bytes to "out". Error message or empty.
std::string advance(ChunkedDecoder& d, std::string& out) {
    using State = ChunkedDecoder::State;
    for (;;) {
        switch (d.state) {
        case State::Size: {
            const std::size_t nl = d.buffer.find('\n');
            if (nl == std::string::npos) {
                return d.buffer.size() > 4096 ? "chunk size line too long" : "";
            }
            std::string_view line(d.buffer.data(), nl);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            line = trim(line.substr(0, line.find(';'))); // chunk extensions are ignored
            if (line.empty() || line.size() > 15) {
                return "invalid chunk size";
            }
            std::uint64_t size = 0;
            for (const char c : line) {
                const int v = hexValue(c);
                if (v < 0) {
                    return "invalid chunk size";
                }
                size = size * 16 + static_cast<std::uint64_t>(v);
            }
            d.buffer.erase(0, nl + 1);
            d.remaining = size;
            d.state = size == 0 ? State::Trailers : State::Data;
            break;
        }
        case State::Data: {
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(d.remaining, d.buffer.size()));
            out.append(d.buffer, 0, take);
            d.buffer.erase(0, take);
            d.remaining -= take;
            if (d.remaining > 0) {
                return "";
            }
            d.state = State::DataEnd;
            break;
        }
        case State::DataEnd:
            if (d.buffer.size() >= 2 && d.buffer[0] == '\r' && d.buffer[1] == '\n') {
                d.buffer.erase(0, 2);
            } else if (!d.buffer.empty() && d.buffer[0] == '\n') {
                d.buffer.erase(0, 1);
            } else if (d.buffer.empty() || (d.buffer.size() == 1 && d.buffer[0] == '\r')) {
                return "";
            } else {
                return "missing line break after a chunk";
            }
            d.state = State::Size;
            break;
        case State::Trailers: {
            const std::size_t nl = d.buffer.find('\n');
            if (nl == std::string::npos) {
                return d.trailerBytes + d.buffer.size() > d.maxTrailerSize ? "trailers too large" : "";
            }
            std::string_view line(d.buffer.data(), nl);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            d.trailerBytes += nl + 1;
            if (d.trailerBytes > d.maxTrailerSize) {
                return "trailers too large";
            }
            if (line.empty()) {
                d.buffer.erase(0, nl + 1);
                d.state = State::Done;
                return "";
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos || !isToken(line.substr(0, colon))) {
                return "invalid trailer line";
            }
            d.trailers.emplace_back(std::string(line.substr(0, colon)), std::string(trim(line.substr(colon + 1))));
            d.buffer.erase(0, nl + 1);
            break;
        }
        case State::Done:
        case State::Failed:
            return "";
        }
    }
}

// net.http.chunkedDecoder(options: { maxTrailerSize: number? }?): ChunkedDecoder
int httpChunkedDecoder(lua_State* L) {
    std::size_t maxTrailers = 16 * 1024;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_getfield(L, 1, "maxTrailerSize");
        if (!lua_isnil(L, -1)) {
            maxTrailers = static_cast<std::size_t>(std::max(0.0, luaL_checknumber(L, -1)));
        }
        lua_pop(L, 1);
    }
    void* memory = lua_newuserdatadtor(L, sizeof(ChunkedDecoder), [](lua_State*, void* p) { static_cast<ChunkedDecoder*>(p)->~ChunkedDecoder(); });
    auto* decoder = new (memory) ChunkedDecoder();
    decoder->maxTrailerSize = maxTrailers;
    luaL_getmetatable(L, kDecoderMeta);
    lua_setmetatable(L, -2);
    return 1;
}

// decoder:push(data: string): (string, boolean) | (nil, string)   body bytes so far, and whether the body ended
int decoderPush(lua_State* L) {
    ChunkedDecoder* d = checkDecoder(L);
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 2, &length);
    if (d->state == ChunkedDecoder::State::Failed) {
        return failure(L, "the decoder failed earlier");
    }
    d->buffer.append(data, length);

    std::string out;
    const std::string err = d->state == ChunkedDecoder::State::Done ? std::string() : advance(*d, out);
    if (!err.empty()) {
        d->state = ChunkedDecoder::State::Failed;
        return failure(L, err);
    }
    pushString(L, out);
    lua_pushboolean(L, d->state == ChunkedDecoder::State::Done);
    return 2;
}

// decoder:done(): boolean
int decoderDone(lua_State* L) {
    lua_pushboolean(L, checkDecoder(L)->state == ChunkedDecoder::State::Done);
    return 1;
}

// decoder:rest(): string   the bytes after the end of the body (the next message), taken out
int decoderRest(lua_State* L) {
    ChunkedDecoder* d = checkDecoder(L);
    if (d->state != ChunkedDecoder::State::Done) {
        lua_pushliteral(L, "");
        return 1;
    }
    pushString(L, d->buffer);
    d->buffer.clear();
    return 1;
}

// decoder:trailers(): { [string]: string }
int decoderTrailers(lua_State* L) {
    ChunkedDecoder* d = checkDecoder(L);
    lua_createtable(L, 0, 0);
    pushHeaders(L, d->trailers);
    lua_getfield(L, -1, "headers");
    return 1;
}

// Dates //////////////////////////////////////////////////////////////////////

std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string formatDate(double seconds) {
    const auto whole = static_cast<std::int64_t>(std::floor(seconds));
    std::int64_t days = whole / 86400;
    std::int64_t rest = whole % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(days, y, m, d);
    const int weekday = static_cast<int>(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday
    char text[64];
    std::snprintf(text, sizeof text, "%s, %02u %s %04d %02d:%02d:%02d GMT", kDays[weekday], d, kMonths[m - 1], y,
                  static_cast<int>(rest / 3600), static_cast<int>(rest / 60 % 60), static_cast<int>(rest % 60));
    return text;
}

int monthIndex(const char* name) {
    for (int i = 0; i < 12; ++i) {
        if (lowercase(name) == lowercase(kMonths[i])) {
            return i + 1;
        }
    }
    return 0;
}

// IMF-fixdate, RFC 850 and asctime formats (RFC 9110 5.6.7).
std::optional<double> parseDate(const std::string& text) {
    char weekday[16] = {};
    char month[8] = {};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    // Sun, 06 Nov 1994 08:49:37 GMT
    if (std::sscanf(text.c_str(), "%15[A-Za-z], %d %3s %d %d:%d:%d GMT", weekday, &day, month, &year, &hour, &minute, &second) == 7) {
    } else if (std::sscanf(text.c_str(), "%15[A-Za-z], %d-%3s-%d %d:%d:%d GMT", weekday, &day, month, &year, &hour, &minute, &second) == 7) {
        year += year < 70 ? 2000 : (year < 100 ? 1900 : 0); // Sunday, 06-Nov-94 08:49:37 GMT
    } else if (std::sscanf(text.c_str(), "%15[A-Za-z] %3s %d %d:%d:%d %d", weekday, month, &day, &hour, &minute, &second, &year) == 7) {
        // Sun Nov  6 08:49:37 1994
    } else {
        return std::nullopt;
    }
    const int m = monthIndex(month);
    if (m == 0 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }
    return static_cast<double>(daysFromCivil(year, static_cast<unsigned>(m), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
                               minute * 60 + second);
}

// net.http.formatDate(time: number?): string   (default: now)
int httpFormatDate(lua_State* L) {
    const double seconds = lua_isnoneornil(L, 1) ? static_cast<double>(std::time(nullptr)) : luaL_checknumber(L, 1);
    pushString(L, formatDate(seconds));
    return 1;
}

// net.http.parseDate(text: string): number?
int httpParseDate(lua_State* L) {
    const std::optional<double> seconds = parseDate(luaL_checkstring(L, 1));
    if (seconds) {
        lua_pushnumber(L, *seconds);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// Header values with parameters ///////////////////////////////////////////////

// "text/html; charset=utf-8" -> "text/html", { charset = "utf-8" }
void splitParams(std::string_view value, std::string& main, std::vector<Header>& params) {
    std::size_t pos = value.find(';');
    main = std::string(trim(value.substr(0, pos)));
    while (pos != std::string_view::npos) {
        ++pos;
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) {
            ++pos;
        }
        const std::size_t eq = value.find_first_of("=;", pos);
        const std::string name = lowercase(trim(value.substr(pos, eq == std::string_view::npos ? std::string_view::npos : eq - pos)));
        std::string paramValue;
        if (eq != std::string_view::npos && value[eq] == '=') {
            pos = eq + 1;
            if (pos < value.size() && value[pos] == '"') {
                ++pos;
                while (pos < value.size() && value[pos] != '"') {
                    if (value[pos] == '\\' && pos + 1 < value.size()) {
                        ++pos;
                    }
                    paramValue += value[pos++];
                }
                pos = value.find(';', pos);
            } else {
                const std::size_t end = value.find(';', pos);
                paramValue = std::string(trim(value.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos)));
                pos = end;
            }
        } else {
            pos = eq;
        }
        if (!name.empty()) {
            params.emplace_back(name, paramValue);
        }
    }
}

void pushParams(lua_State* L, const std::vector<Header>& params) {
    lua_createtable(L, 0, static_cast<int>(params.size()));
    for (const auto& [name, value] : params) {
        lua_getfield(L, -1, name.c_str());
        const bool seen = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!seen) {
            setStringField(L, name.c_str(), value);
        }
    }
}

// net.http.parseHeaderValue(value: string): (string, { [string]: string })
int httpParseHeaderValue(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    std::string main;
    std::vector<Header> params;
    splitParams(std::string_view(text, length), main, params);
    pushString(L, main);
    pushParams(L, params);
    return 2;
}

// net.http.parseList(value: string): { string }
int httpParseList(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const std::vector<std::string> items = splitList(std::string_view(text, length));
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        pushString(L, items[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// net.http.parseQuality(value: string): { { value: string, q: number, params: { [string]: string } } }
// Sorted by q, highest first (ties keep their order). Handy for Accept / Accept-Encoding.
int httpParseQuality(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);

    struct Item {
        std::string value;
        double q;
        std::vector<Header> params;
    };
    std::vector<Item> items;
    for (const std::string& entry : splitList(std::string_view(text, length))) {
        Item item;
        std::vector<Header> params;
        splitParams(entry, item.value, params);
        item.q = 1.0;
        for (const auto& [name, value] : params) {
            if (name == "q") {
                char* end = nullptr;
                const double q = std::strtod(value.c_str(), &end);
                item.q = (end != value.c_str() && q >= 0 && q <= 1) ? q : 0;
            } else {
                item.params.emplace_back(name, value);
            }
        }
        items.push_back(std::move(item));
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.q > b.q; });

    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        lua_createtable(L, 0, 3);
        setStringField(L, "value", items[i].value);
        setNumberField(L, "q", items[i].q);
        pushParams(L, items[i].params);
        lua_setfield(L, -2, "params");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// Authentication /////////////////////////////////////////////////////////////

// net.http.basicAuth(username: string, password: string): string   "Basic dXNlcjpwYXNz"
int httpBasicAuth(lua_State* L) {
    std::size_t userLength = 0;
    std::size_t passLength = 0;
    const char* user = luaL_checklstring(L, 1, &userLength);
    const char* pass = luaL_checklstring(L, 2, &passLength);
    luaL_argcheck(L, std::string_view(user, userLength).find(':') == std::string_view::npos, 1, "username must not contain ':'");
    pushString(L, "Basic " + base64Encode(std::string(user, userLength) + ":" + std::string(pass, passLength)));
    return 1;
}

// net.http.parseBasicAuth(header: string): (string, string) | nil
int httpParseBasicAuth(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    std::string_view value = trim(std::string_view(text, length));
    if (value.size() < 6 || lowercase(value.substr(0, 6)) != "basic ") {
        lua_pushnil(L);
        return 1;
    }
    const std::optional<std::string> decoded = base64Decode(trim(value.substr(6)));
    const std::size_t colon = decoded ? decoded->find(':') : std::string::npos;
    if (colon == std::string::npos) {
        lua_pushnil(L);
        return 1;
    }
    pushString(L, std::string_view(*decoded).substr(0, colon));
    pushString(L, std::string_view(*decoded).substr(colon + 1));
    return 2;
}

// Cookies ////////////////////////////////////////////////////////////////////

std::string_view unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// net.http.parseCookies(header: string): { [string]: string }   (the first of a repeated name wins)
int httpParseCookies(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    std::string_view rest(text, length);
    lua_createtable(L, 0, 0);
    while (!rest.empty()) {
        const std::size_t semi = rest.find(';');
        const std::string_view pair = trim(rest.substr(0, semi));
        rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
        const std::size_t eq = pair.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            continue;
        }
        const std::string name(trim(pair.substr(0, eq)));
        lua_getfield(L, -1, name.c_str());
        const bool seen = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!seen) {
            setStringField(L, name.c_str(), unquote(trim(pair.substr(eq + 1))));
        }
    }
    return 1;
}

// net.http.parseSetCookie(header: string): Cookie | (nil, string)
int httpParseSetCookie(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    std::string_view rest(text, length);

    const std::size_t semi = rest.find(';');
    const std::string_view pair = trim(rest.substr(0, semi));
    const std::size_t eq = pair.find('=');
    if (eq == std::string_view::npos || trim(pair.substr(0, eq)).empty()) {
        return failure(L, "a cookie needs name=value");
    }
    lua_createtable(L, 0, 8);
    setStringField(L, "name", trim(pair.substr(0, eq)));
    setStringField(L, "value", unquote(trim(pair.substr(eq + 1))));
    setBoolField(L, "secure", false);
    setBoolField(L, "httpOnly", false);

    rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
    while (!rest.empty()) {
        const std::size_t next = rest.find(';');
        const std::string_view attribute = trim(rest.substr(0, next));
        rest = next == std::string_view::npos ? std::string_view() : rest.substr(next + 1);
        const std::size_t aeq = attribute.find('=');
        const std::string name = lowercase(trim(attribute.substr(0, aeq)));
        const std::string_view value = aeq == std::string_view::npos ? std::string_view() : trim(attribute.substr(aeq + 1));

        if (name == "expires") {
            if (const std::optional<double> when = parseDate(std::string(value))) {
                setNumberField(L, "expires", *when);
            }
        } else if (name == "max-age") {
            const std::string digits(value);
            char* end = nullptr;
            const double seconds = std::strtod(digits.c_str(), &end);
            if (!digits.empty() && *end == '\0') {
                setNumberField(L, "maxAge", std::floor(seconds));
            }
        } else if (name == "domain" && !value.empty()) {
            setStringField(L, "domain", lowercase(value.front() == '.' ? value.substr(1) : value));
        } else if (name == "path" && !value.empty() && value.front() == '/') {
            setStringField(L, "path", value);
        } else if (name == "secure") {
            setBoolField(L, "secure", true);
        } else if (name == "httponly") {
            setBoolField(L, "httpOnly", true);
        } else if (name == "samesite") {
            const std::string mode = lowercase(value);
            setStringField(L, "sameSite", mode == "strict" ? "Strict" : mode == "none" ? "None" : "Lax");
        } else if (name == "partitioned") {
            setBoolField(L, "partitioned", true);
        }
    }
    return 1;
}

bool validCookieValue(std::string_view value) {
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

// net.http.formatSetCookie(cookie: Cookie): string
int httpFormatSetCookie(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* where = "formatSetCookie";
    const std::string name(optStringField(L, 1, "name", where).value_or(""));
    const std::string value(optStringField(L, 1, "value", where).value_or(""));
    if (!isToken(name)) {
        luaL_error(L, "%s: invalid cookie name '%s'", where, name.c_str());
    }
    if (!validCookieValue(value)) {
        luaL_error(L, "%s: the cookie value has characters that are not allowed (encode it first)", where);
    }
    std::string out = name + "=" + value;

    lua_getfield(L, 1, "expires");
    if (lua_isnumber(L, -1)) {
        out += "; Expires=" + formatDate(lua_tonumber(L, -1));
    } else if (lua_isstring(L, -1)) {
        out += "; Expires=" + std::string(lua_tostring(L, -1));
    }
    lua_getfield(L, 1, "maxAge");
    if (lua_isnumber(L, -1)) {
        out += "; Max-Age=" + std::to_string(static_cast<long long>(std::floor(lua_tonumber(L, -1))));
    }
    for (const char* field : {"domain", "path"}) {
        if (const auto text = optStringField(L, 1, field, where)) {
            if (text->find_first_of(";\r\n") != std::string_view::npos) {
                luaL_error(L, "%s: invalid %s", where, field);
            }
            out += std::string("; ") + (field[0] == 'd' ? "Domain=" : "Path=") + std::string(*text);
        }
    }
    lua_getfield(L, 1, "secure");
    if (lua_toboolean(L, -1)) {
        out += "; Secure";
    }
    lua_getfield(L, 1, "httpOnly");
    if (lua_toboolean(L, -1)) {
        out += "; HttpOnly";
    }
    if (const auto sameSite = optStringField(L, 1, "sameSite", where)) {
        const std::string mode = lowercase(*sameSite);
        if (mode != "strict" && mode != "lax" && mode != "none") {
            luaL_error(L, "%s: sameSite must be \"Strict\", \"Lax\" or \"None\"", where);
        }
        out += mode == "strict" ? "; SameSite=Strict" : mode == "lax" ? "; SameSite=Lax" : "; SameSite=None";
    }
    lua_getfield(L, 1, "partitioned");
    if (lua_toboolean(L, -1)) {
        out += "; Partitioned";
    }
    pushString(L, out);
    return 1;
}

// net.http.formatCookies(cookies: { [string]: string } | { { string } }): string   "a=1; b=2"
int httpFormatCookies(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<Header> cookies;
    const int n = static_cast<int>(lua_objlen(L, 1));
    if (n > 0) {
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            if (!lua_isstring(L, -2) || !lua_isstring(L, -1)) {
                luaL_error(L, "formatCookies: entries must be { name, value }");
            }
            cookies.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            lua_pop(L, 3);
        }
    } else {
        lua_pushnil(L);
        while (lua_next(L, 1) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING || !lua_isstring(L, -1)) {
                luaL_error(L, "formatCookies: expected string names and values");
            }
            cookies.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        std::sort(cookies.begin(), cookies.end());
    }
    std::string out;
    for (const auto& [name, value] : cookies) {
        if (!isToken(name) || !validCookieValue(value)) {
            luaL_error(L, "formatCookies: invalid cookie '%s'", name.c_str());
        }
        if (!out.empty()) {
            out += "; ";
        }
        out += name + "=" + value;
    }
    pushString(L, out);
    return 1;
}

// Multipart ////////////////////////////////////////////////////////////////

std::string escapeQuoted(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"') {
            out += "%22";
        } else if (c == '\r') {
            out += "%0D";
        } else if (c == '\n') {
            out += "%0A";
        } else {
            out += c;
        }
    }
    return out;
}

// net.http.encodeMultipart(parts: { Part }, boundary: string?): (string, string)   body, content type
//     Part = { name: string, value: string, filename: string?, contentType: string?, headers: Headers? }
int httpEncodeMultipart(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string boundary;
    if (lua_isnoneornil(L, 2)) {
        static constexpr char kHex[] = "0123456789abcdef";
        boundary = "----SonataBoundary";
        for (const char c : randomBytes(12)) {
            boundary += kHex[static_cast<unsigned char>(c) >> 4];
            boundary += kHex[static_cast<unsigned char>(c) & 15];
        }
    } else {
        boundary = luaL_checkstring(L, 2);
        luaL_argcheck(L, !boundary.empty() && boundary.size() <= 70 && validFieldValue(boundary), 2, "invalid boundary");
    }

    std::string body;
    const int n = static_cast<int>(lua_objlen(L, 1));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        if (!lua_istable(L, -1)) {
            luaL_error(L, "encodeMultipart: part %d must be a table", i);
        }
        const int part = lua_gettop(L);
        const auto name = optStringField(L, part, "name", "encodeMultipart");
        const auto value = optStringField(L, part, "value", "encodeMultipart");
        if (!name || !value) {
            luaL_error(L, "encodeMultipart: part %d needs a name and a value", i);
        }
        const auto filename = optStringField(L, part, "filename", "encodeMultipart");
        const auto contentType = optStringField(L, part, "contentType", "encodeMultipart");
        const std::vector<Header> extra = readHeaders(L, part, "encodeMultipart");

        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + escapeQuoted(*name) + "\"";
        if (filename) {
            body += "; filename=\"" + escapeQuoted(*filename) + "\"";
        }
        body += "\r\n";
        if (contentType) {
            body += "Content-Type: " + std::string(*contentType) + "\r\n";
        } else if (filename) {
            body += "Content-Type: application/octet-stream\r\n";
        }
        appendHeaders(body, extra);
        body += *value;
        body += "\r\n";
        lua_settop(L, part - 1);
    }
    body += "--" + boundary + "--\r\n";

    pushString(L, body);
    pushString(L, "multipart/form-data; boundary=" + boundary);
    return 2;
}

// net.http.decodeMultipart(body: string, boundaryOrContentType: string): { Part } | (nil, string)
int httpDecodeMultipart(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const std::string_view body(text, length);
    std::string boundary = luaL_checkstring(L, 2);
    if (boundary.find('=') != std::string::npos) { // a whole Content-Type value
        std::string main;
        std::vector<Header> params;
        splitParams(boundary, main, params);
        boundary.clear();
        for (const auto& [name, value] : params) {
            if (name == "boundary") {
                boundary = value;
            }
        }
    }
    if (boundary.empty()) {
        return failure(L, "no boundary");
    }

    const std::string delimiter = "--" + boundary;
    std::size_t pos = body.find(delimiter);
    if (pos == std::string_view::npos) {
        return failure(L, "the boundary does not appear in the body");
    }

    lua_createtable(L, 0, 0);
    int count = 0;
    for (;;) {
        pos += delimiter.size();
        if (body.substr(pos, 2) == "--") {
            return 1; // the closing delimiter
        }
        if (body.substr(pos, 2) == "\r\n") {
            pos += 2;
        } else if (body.substr(pos, 1) == "\n") {
            pos += 1;
        } else {
            return failure(L, "malformed multipart body");
        }

        Head head;
        std::size_t consumed = 0;
        std::string err;
        // Part headers look like a message head without the first line: give it one.
        const std::string fake = "GET / HTTP/1.1\r\n" + std::string(body.substr(pos, std::min<std::size_t>(body.size() - pos, kDefaultMaxHead)));
        if (parseHead(fake, false, kDefaultMaxHead, kDefaultMaxHeaders, head, consumed, err) != Parse::Done) {
            return failure(L, "malformed part headers");
        }
        pos += consumed - 16;

        const std::size_t end = body.find("\r\n" + delimiter, pos);
        const std::size_t looseEnd = end == std::string_view::npos ? body.find("\n" + delimiter, pos) : end;
        if (looseEnd == std::string_view::npos) {
            return failure(L, "the multipart body is cut short");
        }
        const std::string_view value = body.substr(pos, looseEnd - pos);

        lua_createtable(L, 0, 6);
        pushHeaders(L, head.headers);
        setStringField(L, "value", value);
        for (const auto& [name, headerValue] : head.headers) {
            const std::string lower = lowercase(name);
            if (lower == "content-disposition") {
                std::string main;
                std::vector<Header> params;
                splitParams(headerValue, main, params);
                for (const auto& [param, paramValue] : params) {
                    if (param == "name" || param == "filename") {
                        setStringField(L, param.c_str(), paramValue);
                    }
                }
            } else if (lower == "content-type") {
                setStringField(L, "contentType", headerValue);
            }
        }
        lua_rawseti(L, -2, ++count);

        pos = looseEnd + (end == std::string_view::npos ? 1 : 2);
    }
}

// Media types ////////////////////////////////////////////////////////////////

struct MediaType {
    const char* extension;
    const char* type;
};

constexpr MediaType kMediaTypes[] = {
    {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"},
    {"mjs", "text/javascript"}, {"json", "application/json"}, {"map", "application/json"},
    {"jsonld", "application/ld+json"}, {"webmanifest", "application/manifest+json"}, {"txt", "text/plain"},
    {"md", "text/markdown"}, {"csv", "text/csv"}, {"tsv", "text/tab-separated-values"}, {"xml", "application/xml"},
    {"xhtml", "application/xhtml+xml"}, {"rss", "application/rss+xml"}, {"atom", "application/atom+xml"},
    {"yaml", "application/yaml"}, {"yml", "application/yaml"}, {"toml", "application/toml"},
    {"ics", "text/calendar"}, {"rtf", "application/rtf"}, {"svg", "image/svg+xml"}, {"png", "image/png"},
    {"apng", "image/apng"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
    {"webp", "image/webp"}, {"avif", "image/avif"}, {"heic", "image/heic"}, {"jxl", "image/jxl"},
    {"ico", "image/x-icon"}, {"bmp", "image/bmp"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
    {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"}, {"oga", "audio/ogg"},
    {"opus", "audio/opus"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"}, {"flac", "audio/flac"},
    {"weba", "audio/webm"}, {"mid", "audio/midi"}, {"midi", "audio/midi"}, {"mp4", "video/mp4"},
    {"m4v", "video/mp4"}, {"webm", "video/webm"}, {"ogv", "video/ogg"}, {"mov", "video/quicktime"},
    {"avi", "video/x-msvideo"}, {"mkv", "video/x-matroska"}, {"mpeg", "video/mpeg"}, {"ts", "video/mp2t"},
    {"pdf", "application/pdf"}, {"zip", "application/zip"}, {"gz", "application/gzip"},
    {"tgz", "application/gzip"}, {"bz2", "application/x-bzip2"}, {"xz", "application/x-xz"},
    {"zst", "application/zstd"}, {"7z", "application/x-7z-compressed"}, {"rar", "application/vnd.rar"},
    {"tar", "application/x-tar"}, {"wasm", "application/wasm"}, {"woff", "font/woff"}, {"woff2", "font/woff2"},
    {"ttf", "font/ttf"}, {"otf", "font/otf"}, {"eot", "application/vnd.ms-fontobject"},
    {"doc", "application/msword"}, {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"xls", "application/vnd.ms-excel"}, {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"odt", "application/vnd.oasis.opendocument.text"}, {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
    {"odp", "application/vnd.oasis.opendocument.presentation"}, {"epub", "application/epub+zip"},
    {"jar", "application/java-archive"}, {"apk", "application/vnd.android.package-archive"},
    {"exe", "application/vnd.microsoft.portable-executable"}, {"msi", "application/x-msi"},
    {"deb", "application/vnd.debian.binary-package"}, {"rpm", "application/x-rpm"},
    {"dmg", "application/x-apple-diskimage"}, {"iso", "application/x-iso9660-image"},
    {"bin", "application/octet-stream"}, {"sh", "application/x-sh"}, {"py", "text/x-python"},
    {"lua", "text/x-lua"}, {"luau", "text/x-luau"}, {"c", "text/x-c"}, {"h", "text/x-c"},
    {"cpp", "text/x-c++"}, {"hpp", "text/x-c++"}, {"rs", "text/x-rust"}, {"go", "text/x-go"},
    {"java", "text/x-java"}, {"tsx", "text/tsx"}, {"jsx", "text/jsx"}, {"sql", "application/sql"},
    {"graphql", "application/graphql"}, {"gql", "application/graphql"},
};

// net.mime.lookup(pathOrExtension: string): string?
int mimeLookup(lua_State* L) {
    std::string text = luaL_checkstring(L, 1);
    const std::size_t slash = text.find_last_of("/\\");
    if (slash != std::string::npos) {
        text.erase(0, slash + 1);
    }
    const std::size_t dot = text.rfind('.');
    const std::string extension = lowercase(dot == std::string::npos ? text : text.substr(dot + 1));
    for (const MediaType& type : kMediaTypes) {
        if (extension == type.extension) {
            lua_pushstring(L, type.type);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// net.mime.extension(type: string): string?   ("text/html; charset=utf-8" works too)
int mimeExtension(lua_State* L) {
    std::string main;
    std::vector<Header> params;
    splitParams(luaL_checkstring(L, 1), main, params);
    const std::string essence = lowercase(main);
    for (const MediaType& type : kMediaTypes) {
        if (essence == type.type) {
            lua_pushstring(L, type.extension);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// net.mime.parse(value: string): { type, subtype, essence, params } | (nil, string)
int mimeParse(lua_State* L) {
    std::string main;
    std::vector<Header> params;
    splitParams(luaL_checkstring(L, 1), main, params);
    const std::size_t slash = main.find('/');
    if (slash == std::string::npos || !isToken(main.substr(0, slash)) || !isToken(main.substr(slash + 1))) {
        return failure(L, "not a media type");
    }
    const std::string essence = lowercase(main);
    lua_createtable(L, 0, 4);
    setStringField(L, "type", essence.substr(0, slash));
    setStringField(L, "subtype", essence.substr(slash + 1));
    setStringField(L, "essence", essence);
    pushParams(L, params);
    lua_setfield(L, -2, "params");
    return 1;
}

// net.mime.isText(type: string): boolean   text/*, JSON, XML, JavaScript and friends
int mimeIsText(lua_State* L) {
    std::string main;
    std::vector<Header> params;
    splitParams(luaL_checkstring(L, 1), main, params);
    const std::string essence = lowercase(main);
    const bool text = essence.rfind("text/", 0) == 0 || essence.find("+json") != std::string::npos ||
                      essence.find("+xml") != std::string::npos || essence == "application/json" ||
                      essence == "application/xml" || essence == "application/javascript" ||
                      essence == "application/yaml" || essence == "application/toml" || essence == "application/sql" ||
                      essence == "application/graphql" || essence == "image/svg+xml";
    lua_pushboolean(L, text);
    return 1;
}

const Function kHttpFunctions[] = {
    {"parseRequest", httpParseRequest},
    {"parseResponse", httpParseResponse},
    {"formatRequest", httpFormatRequest},
    {"formatResponse", httpFormatResponse},
    {"statusText", httpStatusText},
    {"bodyLength", httpBodyLength},
    {"keepAlive", httpKeepAlive},
    {"encodeChunk", httpEncodeChunk},
    {"lastChunk", httpLastChunk},
    {"chunkedDecoder", httpChunkedDecoder},
    {"formatDate", httpFormatDate},
    {"parseDate", httpParseDate},
    {"parseHeaderValue", httpParseHeaderValue},
    {"parseList", httpParseList},
    {"parseQuality", httpParseQuality},
    {"basicAuth", httpBasicAuth},
    {"parseBasicAuth", httpParseBasicAuth},
    {"parseCookies", httpParseCookies},
    {"parseSetCookie", httpParseSetCookie},
    {"formatSetCookie", httpFormatSetCookie},
    {"formatCookies", httpFormatCookies},
    {"encodeMultipart", httpEncodeMultipart},
    {"decodeMultipart", httpDecodeMultipart},
};

const Function kDecoderMethods[] = {
    {"push", decoderPush},
    {"done", decoderDone},
    {"rest", decoderRest},
    {"trailers", decoderTrailers},
};

const Function kMimeFunctions[] = {
    {"lookup", mimeLookup},
    {"extension", mimeExtension},
    {"parse", mimeParse},
    {"isText", mimeIsText},
};

} // namespace

void pushHttp(lua_State* L) {
    registerType(L, kDecoderMeta, "net.ChunkedDecoder", kDecoderMethods);
    lua_createtable(L, 0, static_cast<int>(std::size(kHttpFunctions)));
    setFunctionList(L, kHttpFunctions);
}

void pushMime(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kMimeFunctions)));
    setFunctionList(L, kMimeFunctions);
}

} // namespace sonata::lib::libs::net
