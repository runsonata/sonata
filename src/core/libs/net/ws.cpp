// @sonata/net WebSocket framing (RFC 6455): the handshake keys and the frame format.
// No I/O: frames go over any socket, plain or TLS.

#include "common.hpp"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs::net {

namespace {

constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct Opcode {
    const char* name;
    int code;
};

constexpr Opcode kOpcodes[] = {
    {"continuation", 0}, {"text", 1}, {"binary", 2}, {"close", 8}, {"ping", 9}, {"pong", 10},
};

const char* opcodeName(int code) {
    for (const Opcode& op : kOpcodes) {
        if (op.code == code) {
            return op.name;
        }
    }
    return nullptr;
}

// net.ws.acceptKey(key: string): string   the Sec-WebSocket-Accept value for a Sec-WebSocket-Key
int wsAcceptKey(lua_State* L) {
    std::size_t length = 0;
    const char* key = luaL_checklstring(L, 1, &length);
    pushString(L, base64Encode(sha1(std::string(key, length) + kGuid)));
    return 1;
}

// net.ws.generateKey(): string   a random Sec-WebSocket-Key
int wsGenerateKey(lua_State* L) {
    pushString(L, base64Encode(randomBytes(16)));
    return 1;
}

void applyMask(std::string& data, std::size_t from, const unsigned char* mask) {
    for (std::size_t i = from; i < data.size(); ++i) {
        data[i] = static_cast<char>(static_cast<unsigned char>(data[i]) ^ mask[(i - from) % 4]);
    }
}

// net.ws.encode(opcode: string | number, payload: string?, options: { mask: boolean | string?, fin: boolean?,
//               rsv1: boolean? }?): string
int wsEncode(lua_State* L) {
    int opcode = -1;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        opcode = luaL_checkinteger(L, 1);
        luaL_argcheck(L, opcode >= 0 && opcode <= 15, 1, "opcode must be between 0 and 15");
    } else {
        const char* name = luaL_checkstring(L, 1);
        for (const Opcode& op : kOpcodes) {
            if (std::strcmp(name, op.name) == 0) {
                opcode = op.code;
            }
        }
        luaL_argcheck(L, opcode >= 0, 1, "unknown opcode");
    }
    std::size_t length = 0;
    const char* payload = luaL_optlstring(L, 2, "", &length);

    bool fin = true;
    bool rsv1 = false;
    std::string mask;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "fin");
        fin = lua_isnil(L, -1) || lua_toboolean(L, -1) != 0;
        lua_getfield(L, 3, "rsv1");
        rsv1 = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 3, "mask");
        if (lua_type(L, -1) == LUA_TSTRING) {
            std::size_t maskLength = 0;
            const char* given = lua_tolstring(L, -1, &maskLength);
            luaL_argcheck(L, maskLength == 4, 3, "mask must be 4 bytes");
            mask.assign(given, 4);
        } else if (lua_toboolean(L, -1)) {
            mask = randomBytes(4);
        }
    }
    luaL_argcheck(L, opcode < 8 || (length <= 125 && fin), 2, "control frames carry at most 125 bytes and cannot be fragmented");

    std::string frame;
    frame += static_cast<char>((fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | opcode);
    const char maskBit = mask.empty() ? 0 : static_cast<char>(0x80);
    if (length < 126) {
        frame += static_cast<char>(maskBit | static_cast<char>(length));
    } else if (length <= 0xFFFF) {
        frame += static_cast<char>(maskBit | 126);
        frame += static_cast<char>((length >> 8) & 0xFF);
        frame += static_cast<char>(length & 0xFF);
    } else {
        frame += static_cast<char>(maskBit | 127);
        for (int i = 7; i >= 0; --i) {
            frame += static_cast<char>((static_cast<std::uint64_t>(length) >> (i * 8)) & 0xFF);
        }
    }
    frame += mask;
    const std::size_t start = frame.size();
    frame.append(payload, length);
    if (!mask.empty()) {
        applyMask(frame, start, reinterpret_cast<const unsigned char*>(mask.data()));
    }
    pushString(L, frame);
    return 1;
}

// net.ws.decode(data: string, options: { maxPayload: number? }?):
//     (Frame, number) | nil (need more data) | (nil, string)
//     Frame = { opcode: string | number, fin, rsv1, rsv2, rsv3, masked: boolean, payload: string }
int wsDecode(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    double maxPayload = 64.0 * 1024 * 1024;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "maxPayload");
        if (!lua_isnil(L, -1)) {
            maxPayload = luaL_checknumber(L, -1);
        }
        lua_pop(L, 1);
    }

    const auto* data = reinterpret_cast<const unsigned char*>(text);
    if (length < 2) {
        lua_pushnil(L);
        return 1;
    }
    const int opcode = data[0] & 0x0F;
    const bool fin = (data[0] & 0x80) != 0;
    const bool masked = (data[1] & 0x80) != 0;
    std::uint64_t payloadLength = data[1] & 0x7F;
    std::size_t pos = 2;
    if (payloadLength == 126) {
        if (length < 4) {
            lua_pushnil(L);
            return 1;
        }
        payloadLength = (static_cast<std::uint64_t>(data[2]) << 8) | data[3];
        pos = 4;
    } else if (payloadLength == 127) {
        if (length < 10) {
            lua_pushnil(L);
            return 1;
        }
        payloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLength = (payloadLength << 8) | data[2 + i];
        }
        if (payloadLength >> 63) {
            return failure(L, "invalid payload length");
        }
        pos = 10;
    }
    if (opcode >= 8 && (payloadLength > 125 || !fin)) {
        return failure(L, "invalid control frame");
    }
    if (static_cast<double>(payloadLength) > maxPayload) {
        return failure(L, "payload too large");
    }
    unsigned char mask[4] = {};
    if (masked) {
        if (length < pos + 4) {
            lua_pushnil(L);
            return 1;
        }
        std::memcpy(mask, data + pos, 4);
        pos += 4;
    }
    if (length - pos < payloadLength) {
        lua_pushnil(L);
        return 1;
    }

    std::string payload(text + pos, static_cast<std::size_t>(payloadLength));
    if (masked) {
        applyMask(payload, 0, mask);
    }

    lua_createtable(L, 0, 7);
    if (const char* name = opcodeName(opcode)) {
        setStringField(L, "opcode", name);
    } else {
        setNumberField(L, "opcode", opcode);
    }
    setBoolField(L, "fin", fin);
    setBoolField(L, "rsv1", (data[0] & 0x40) != 0);
    setBoolField(L, "rsv2", (data[0] & 0x20) != 0);
    setBoolField(L, "rsv3", (data[0] & 0x10) != 0);
    setBoolField(L, "masked", masked);
    setStringField(L, "payload", payload);
    lua_pushnumber(L, static_cast<double>(pos + payloadLength));
    return 2;
}

// net.ws.closePayload(code: number?, reason: string?): string
int wsClosePayload(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushliteral(L, "");
        return 1;
    }
    const int code = luaL_checkinteger(L, 1);
    luaL_argcheck(L, code >= 1000 && code <= 4999, 1, "close code must be between 1000 and 4999");
    std::size_t length = 0;
    const char* reason = luaL_optlstring(L, 2, "", &length);
    luaL_argcheck(L, length <= 123, 2, "reason must be at most 123 bytes");
    std::string payload;
    payload += static_cast<char>((code >> 8) & 0xFF);
    payload += static_cast<char>(code & 0xFF);
    payload.append(reason, length);
    pushString(L, payload);
    return 1;
}

// net.ws.parseClose(payload: string): (number?, string)
int wsParseClose(lua_State* L) {
    std::size_t length = 0;
    const char* payload = luaL_checklstring(L, 1, &length);
    if (length < 2) {
        lua_pushnil(L);
        lua_pushliteral(L, "");
        return 2;
    }
    lua_pushnumber(L, (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]));
    lua_pushlstring(L, payload + 2, length - 2);
    return 2;
}

// net.ws.mask(data: string, key: string): string   XORs data with a 4-byte masking key
int wsMask(lua_State* L) {
    std::size_t length = 0;
    std::size_t keyLength = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const char* key = luaL_checklstring(L, 2, &keyLength);
    luaL_argcheck(L, keyLength == 4, 2, "the key must be 4 bytes");
    std::string out(data, length);
    applyMask(out, 0, reinterpret_cast<const unsigned char*>(key));
    pushString(L, out);
    return 1;
}

const Function kWsFunctions[] = {
    {"acceptKey", wsAcceptKey},       {"generateKey", wsGenerateKey}, {"encode", wsEncode},
    {"decode", wsDecode},             {"closePayload", wsClosePayload}, {"parseClose", wsParseClose},
    {"mask", wsMask},
};

} // namespace

void pushWs(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kWsFunctions)) + 1);
    setFunctionList(L, kWsFunctions);
}

} // namespace sonata::lib::libs::net
