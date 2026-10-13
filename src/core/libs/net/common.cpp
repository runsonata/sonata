#include "common.hpp"

#include <array>
#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#ifdef SONATA_NET_TLS
#include <openssl/rand.h>
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs::net {

void ensureNetworking() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

int failure(lua_State* L, std::string_view message) {
    lua_pushnil(L);
    lua_pushlstring(L, message.data(), message.size());
    return 2;
}

void pushString(lua_State* L, std::string_view text) {
    lua_pushlstring(L, text.data(), text.size());
}

void setStringField(lua_State* L, const char* key, std::string_view text) {
    pushString(L, text);
    lua_setfield(L, -2, key);
}

void setNumberField(lua_State* L, const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

void setBoolField(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

std::optional<std::string_view> optStringField(lua_State* L, int index, const char* name, const char* where) {
    lua_getfield(L, index, name);
    if (lua_isnil(L, -1)) {
        return std::nullopt;
    }
    if (!lua_isstring(L, -1)) {
        luaL_error(L, "%s: '%s' must be a string", where, name);
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    return std::string_view(text, length);
}

void setFunctionList(lua_State* L, const Function* functions, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        lua_pushcfunction(L, functions[i].function, functions[i].name);
        lua_setfield(L, -2, functions[i].name);
    }
    lua_setreadonly(L, -1, 1);
}

void registerType(lua_State* L, const char* name, const char* typeName, const Function* methods, std::size_t count) {
    if (luaL_newmetatable(L, name)) {
        lua_createtable(L, 0, static_cast<int>(count));
        setFunctionList(L, methods, count);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, typeName);
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
}

// codecs ///////////////////////////////////////////////////////////////////

namespace {
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}
// THANK YOU CLAUDE
std::string base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                                static_cast<unsigned char>(data[i + 2]);
        out += kBase64[v >> 18];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += kBase64[v & 63];
    }
    const std::size_t rest = data.size() - i;
    if (rest > 0) {
        std::uint32_t v = static_cast<unsigned char>(data[i]) << 16;
        if (rest == 2) {
            v |= static_cast<unsigned char>(data[i + 1]) << 8;
        }
        out += kBase64[v >> 18];
        out += kBase64[(v >> 12) & 63];
        out += rest == 2 ? kBase64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    std::array<int, 256> values{};
    values.fill(-1);
    for (int i = 0; i < 64; ++i) {
        values[static_cast<unsigned char>(kBase64[i])] = i;
    }
    values[static_cast<unsigned char>('-')] = 62; // base64url as well
    values[static_cast<unsigned char>('_')] = 63;

    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    bool padding = false;
    for (const char c : text) {
        if (c == '=') {
            padding = true;
            continue;
        }
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        const int v = values[static_cast<unsigned char>(c)];
        if (v < 0 || padding) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

std::string sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string message(data);
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8;
    message += static_cast<char>(0x80);
    while (message.size() % 64 != 56) {
        message += '\0';
    }
    for (int i = 7; i >= 0; --i) {
        message += static_cast<char>((bitLength >> (i * 8)) & 0xFF);
    }

    const auto rotl = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(message[chunk + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[chunk + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[chunk + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(message[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string digest(20, '\0');
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 4; ++j) {
            digest[static_cast<std::size_t>(i * 4 + j)] = static_cast<char>((h[i] >> (24 - j * 8)) & 0xFF);
        }
    }
    return digest;
}

std::string randomBytes(std::size_t count) {
    std::string out(count, '\0');
#ifdef SONATA_NET_TLS
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) == 1) {
        return out;
    }
#endif
    std::random_device device;
    for (char& c : out) {
        c = static_cast<char>(device() & 0xFF);
    }
    return out;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = toLower(c);
    }
    return out;
}

int hexValue(char c) {
    if (isDigit(c)) {
        return c - '0';
    }
    const char lower = toLower(c);
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

bool isTokenChar(char c) {
    return isAlpha(c) || isDigit(c) || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

bool isToken(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c == '\0' || !isTokenChar(c)) {
            return false;
        }
    }
    return true;
}

void percentEncode(std::string& out, std::string_view text, std::string_view safe) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const char c : text) {
        const bool keep = isAlpha(c) || isDigit(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
                          (c != '\0' && safe.find(c) != std::string_view::npos);
        if (keep) {
            out += c;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 15];
        }
    }
}

bool percentDecode(std::string& out, std::string_view text, bool plusAsSpace) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) {
                return false;
            }
            const int hi = hexValue(text[i + 1]);
            const int lo = hexValue(text[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        } else {
            out += (plusAsSpace && c == '+') ? ' ' : c;
        }
    }
    return true;
}

} // namespace sonata::lib::libs::net
