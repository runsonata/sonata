#pragma once

// Shared by the parts of @sonata/net (src/core/libs/net/*.cpp). Each part
// builds one piece of the library table; net.cpp puts them together.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace sonata::lib::libs::net {

// Pieces of the library. Each pushes one value (a table) onto the stack.
void pushSockets(lua_State* L); // fills the fields of the table on top instead (connect, listen, ...)
void pushDns(lua_State* L);     // net.dns
void pushIp(lua_State* L);      // net.ip
void pushInterfaces(lua_State* L); // fills net.interfaces on the table on top
void pushUrl(lua_State* L);     // net.url
void pushIdna(lua_State* L);    // net.idna
void pushHttp(lua_State* L);    // net.http
void pushMime(lua_State* L);    // net.mime
void pushWs(lua_State* L);      // net.ws
void pushTls(lua_State* L);     // net.tls

// Winsock must be started before any socket call on Windows. Safe to call (i hope)
void ensureNetworking();

// helpers ////////////////////////////////////////////////////////////////////

// Pushes nil, message and returns 2.
int failure(lua_State* L, std::string_view message);

void pushString(lua_State* L, std::string_view text);
void setStringField(lua_State* L, const char* key, std::string_view text);
void setNumberField(lua_State* L, const char* key, double value);
void setBoolField(lua_State* L, const char* key, bool value);

// Optional string field of the table at "index". The value stays on the stack (it keeps the
// string alive); raises "<where>: '<name>' must be a string" for other types.
std::optional<std::string_view> optStringField(lua_State* L, int index, const char* name, const char* where);

// Sets every function as a field of the table on top of the stack, then makes it read-only.
struct Function {
    const char* name;
    int (*function)(lua_State* L);
};
void setFunctionList(lua_State* L, const Function* functions, std::size_t count);
template <std::size_t N>
void setFunctionList(lua_State* L, const Function (&functions)[N]) {
    setFunctionList(L, functions, N);
}

// Creates the metatable "name" (once per VM) with these methods as __index, and
// "typeName" as __type. Leaves the stack as it was.
void registerType(lua_State* L, const char* name, const char* typeName, const Function* methods, std::size_t count);
template <std::size_t N>
void registerType(lua_State* L, const char* name, const char* typeName, const Function (&methods)[N]) {
    registerType(L, name, typeName, methods, N);
}

// codecs ///////////////////////////////////////////////////////////////////

std::string base64Encode(std::string_view data);
std::optional<std::string> base64Decode(std::string_view text);
std::string sha1(std::string_view data); // 20 raw bytes
std::string randomBytes(std::size_t count);

constexpr bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }
constexpr char toLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
std::string lowercase(std::string_view text);
int hexValue(char c);

// HTTP "token" characters (RFC 9110 5.6.2).
bool isTokenChar(char c);
bool isToken(std::string_view text);

// Percent-encoding: everything except A-Z a-z 0-9 - . _ ~ and "safe" becomes %XX.
// Yea no ai did this im not making encoding on mobile
void percentEncode(std::string& out, std::string_view text, std::string_view safe);
bool percentDecode(std::string& out, std::string_view text, bool plusAsSpace);

} // namespace sonata::lib::libs::net
