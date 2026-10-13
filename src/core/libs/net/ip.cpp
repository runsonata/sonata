// @sonata/net IP addresses (net.ip) and the machine's network interfaces (net.interfaces).

#include "common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <netpacket/packet.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <net/if_dl.h>
#endif
#endif

#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs::net {

namespace {

struct Address {
    int version = 4;
    std::array<unsigned char, 16> bytes{}; // the first 4 for IPv4
    std::string zone;                      // IPv6 scope ("eth0" in fe80::1%eth0)

    std::size_t size() const { return version == 4 ? 4 : 16; }
};

std::optional<Address> parseAddress(std::string_view text) {
    Address address;
    std::string plain(text);
    if (const std::size_t percent = plain.find('%'); percent != std::string::npos) {
        address.zone = plain.substr(percent + 1);
        plain.erase(percent);
    }
    if (!plain.empty() && plain.front() == '[' && plain.back() == ']') {
        plain = plain.substr(1, plain.size() - 2);
    }
    if (address.zone.empty() && inet_pton(AF_INET, plain.c_str(), address.bytes.data()) == 1) {
        address.version = 4;
        return address;
    }
    if (inet_pton(AF_INET6, plain.c_str(), address.bytes.data()) == 1) {
        address.version = 6;
        return address;
    }
    return std::nullopt;
}

std::string formatAddress(const Address& address) {
    char text[INET6_ADDRSTRLEN] = {};
    inet_ntop(address.version == 4 ? AF_INET : AF_INET6, address.bytes.data(), text, sizeof text);
    std::string out = text;
    if (!address.zone.empty()) {
        out += '%';
        out += address.zone;
    }
    return out;
}

Address checkAddress(lua_State* L, int idx) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, idx, &length);
    const std::optional<Address> address = parseAddress(std::string_view(text, length));
    if (!address) {
        luaL_argerror(L, idx, lua_pushfstring(L, "'%s' is not an IP address", text));
    }
    return *address;
}

struct Network {
    Address base;
    int prefix = 0;
};

std::optional<Network> parseNetwork(std::string_view text, std::string& error) {
    const std::size_t slash = text.find('/');
    const std::optional<Address> address = parseAddress(text.substr(0, slash));
    if (!address) {
        error = "invalid address";
        return std::nullopt;
    }
    Network network{*address, static_cast<int>(address->size() * 8)};
    if (slash != std::string_view::npos) {
        const std::string digits(text.substr(slash + 1));
        char* end = nullptr;
        const long prefix = std::strtol(digits.c_str(), &end, 10);
        if (digits.empty() || *end != '\0' || prefix < 0 || prefix > network.prefix) {
            error = "invalid prefix length";
            return std::nullopt;
        }
        network.prefix = static_cast<int>(prefix);
    }
    // Clear the host bits.
    for (std::size_t i = 0; i < network.base.size(); ++i) {
        const int bits = std::max(0, std::min(8, network.prefix - static_cast<int>(i) * 8));
        network.base.bytes[i] &= static_cast<unsigned char>(bits == 0 ? 0 : 0xFF << (8 - bits));
    }
    return network;
}

bool contains(const Network& network, const Address& address) {
    if (address.version != network.base.version) {
        return false;
    }
    for (std::size_t i = 0; i < address.size(); ++i) {
        const int bits = std::max(0, std::min(8, network.prefix - static_cast<int>(i) * 8));
        const auto mask = static_cast<unsigned char>(bits == 0 ? 0 : 0xFF << (8 - bits));
        if ((address.bytes[i] & mask) != network.base.bytes[i]) {
            return false;
        }
    }
    return true;
}

bool inNetwork(const Address& address, const char* cidr) {
    std::string error;
    const std::optional<Network> network = parseNetwork(cidr, error);
    return network && contains(*network, address);
}

// What kind of address this is (RFC 6890 and friends).
const char* classify(const Address& input) {
    Address address = input;
    // IPv4-mapped IPv6 (::ffff:a.b.c.d) behaves like the IPv4 address.
    static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (address.version == 6 && std::memcmp(address.bytes.data(), kMapped, 12) == 0) {
        Address v4;
        v4.version = 4;
        std::memcpy(v4.bytes.data(), address.bytes.data() + 12, 4);
        address = v4;
    }

    struct Range {
        const char* cidr;
        const char* kind;
    };
    static const Range kRanges[] = {
        {"0.0.0.0/32", "unspecified"},       {"0.0.0.0/8", "reserved"},         {"10.0.0.0/8", "private"},
        {"100.64.0.0/10", "sharedAddress"},  {"127.0.0.0/8", "loopback"},       {"169.254.0.0/16", "linkLocal"},
        {"172.16.0.0/12", "private"},        {"192.0.0.0/24", "reserved"},      {"192.0.2.0/24", "documentation"},
        {"192.168.0.0/16", "private"},       {"198.18.0.0/15", "benchmarking"}, {"198.51.100.0/24", "documentation"},
        {"203.0.113.0/24", "documentation"}, {"224.0.0.0/4", "multicast"},      {"255.255.255.255/32", "broadcast"},
        {"240.0.0.0/4", "reserved"},         {"::/128", "unspecified"},         {"::1/128", "loopback"},
        {"64:ff9b::/96", "translation"},     {"100::/64", "discard"},           {"2001:db8::/32", "documentation"},
        {"2001::/23", "reserved"},           {"fc00::/7", "private"},           {"fe80::/10", "linkLocal"},
        {"ff00::/8", "multicast"},
    };
    for (const Range& range : kRanges) {
        if (inNetwork(address, range.cidr)) {
            return range.kind;
        }
    }
    return "public";
}

// net.ip.parse(text: string): { version: number, bytes: string, zone: string? } | (nil, string)
int ipParse(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const std::optional<Address> address = parseAddress(std::string_view(text, length));
    if (!address) {
        return failure(L, "not an IP address");
    }
    lua_createtable(L, 0, 3);
    setNumberField(L, "version", address->version);
    lua_pushlstring(L, reinterpret_cast<const char*>(address->bytes.data()), address->size());
    lua_setfield(L, -2, "bytes");
    if (!address->zone.empty()) {
        setStringField(L, "zone", address->zone);
    }
    return 1;
}

// net.ip.version(text: string): number?
int ipVersion(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const std::optional<Address> address = parseAddress(std::string_view(text, length));
    if (address) {
        lua_pushnumber(L, address->version);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// net.ip.isValid(text: string): boolean
int ipIsValid(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    lua_pushboolean(L, parseAddress(std::string_view(text, length)).has_value());
    return 1;
}

// net.ip.normalize(text: string): string | (nil, string)
int ipNormalize(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const std::optional<Address> address = parseAddress(std::string_view(text, length));
    if (!address) {
        return failure(L, "not an IP address");
    }
    pushString(L, formatAddress(*address));
    return 1;
}

// net.ip.fromBytes(bytes: string): string
int ipFromBytes(lua_State* L) {
    std::size_t length = 0;
    const char* bytes = luaL_checklstring(L, 1, &length);
    luaL_argcheck(L, length == 4 || length == 16, 1, "expected 4 (IPv4) or 16 (IPv6) bytes");
    Address address;
    address.version = length == 4 ? 4 : 6;
    std::memcpy(address.bytes.data(), bytes, length);
    pushString(L, formatAddress(address));
    return 1;
}

// net.ip.kind(text: string): string
int ipKind(lua_State* L) {
    lua_pushstring(L, classify(checkAddress(L, 1)));
    return 1;
}

// net.ip.isPublic(text: string): boolean  (routable on the internet)
int ipIsPublic(lua_State* L) {
    lua_pushboolean(L, std::strcmp(classify(checkAddress(L, 1)), "public") == 0);
    return 1;
}

// net.ip.inRange(address: string, cidr: string | { string }): boolean
int ipInRange(lua_State* L) {
    const Address address = checkAddress(L, 1);
    const auto test = [&](int idx) {
        std::size_t length = 0;
        const char* text = luaL_checklstring(L, idx, &length);
        std::string error;
        const std::optional<Network> network = parseNetwork(std::string_view(text, length), error);
        if (!network) {
            luaL_error(L, "net.ip.inRange: '%s': %s", text, error.c_str());
        }
        return contains(*network, address);
    };
    if (lua_istable(L, 2)) {
        const int n = static_cast<int>(lua_objlen(L, 2));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 2, i);
            const bool hit = test(lua_gettop(L));
            lua_pop(L, 1);
            if (hit) {
                lua_pushboolean(L, 1);
                return 1;
            }
        }
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, test(2));
    return 1;
}

// net.ip.parseCidr(cidr: string): { version, prefix, network, first, last, mask?, size } | (nil, string)
int ipParseCidr(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    std::string error;
    const std::optional<Network> network = parseNetwork(std::string_view(text, length), error);
    if (!network) {
        return failure(L, error);
    }

    Address last = network->base;
    for (std::size_t i = 0; i < last.size(); ++i) {
        const int bits = std::max(0, std::min(8, network->prefix - static_cast<int>(i) * 8));
        last.bytes[i] |= static_cast<unsigned char>(bits == 8 ? 0 : 0xFF >> bits);
    }

    lua_createtable(L, 0, 7);
    setNumberField(L, "version", network->base.version);
    setNumberField(L, "prefix", network->prefix);
    setStringField(L, "network", formatAddress(network->base) + "/" + std::to_string(network->prefix));
    setStringField(L, "first", formatAddress(network->base));
    setStringField(L, "last", formatAddress(last));
    const int hostBits = static_cast<int>(network->base.size() * 8) - network->prefix;
    setNumberField(L, "size", std::ldexp(1.0, hostBits));
    if (network->base.version == 4) {
        Address mask;
        mask.version = 4;
        for (int i = 0; i < 4; ++i) {
            const int bits = std::max(0, std::min(8, network->prefix - i * 8));
            mask.bytes[static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits == 0 ? 0 : 0xFF << (8 - bits));
        }
        setStringField(L, "mask", formatAddress(mask));
    }
    return 1;
}

// Interfaces /////////////////////////////////////////////////////////////////

struct Interface {
    std::string name;
    std::string address;
    std::string family;
    std::string netmask;
    int prefix = 0;
    bool internal = false;
    std::string mac;
};

int prefixLength(const unsigned char* mask, std::size_t size) {
    int bits = 0;
    for (std::size_t i = 0; i < size; ++i) {
        for (int b = 7; b >= 0; --b) {
            if ((mask[i] >> b) & 1) {
                ++bits;
            }
        }
    }
    return bits;
}

std::string macText(const unsigned char* bytes, std::size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (std::size_t i = 0; i < length; ++i) {
        if (i > 0) {
            text += ':';
        }
        text += kHex[bytes[i] >> 4];
        text += kHex[bytes[i] & 15];
    }
    return text;
}

std::vector<Interface> listInterfaces() {
    std::vector<Interface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        return out;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr; adapter = adapter->Next) {
        std::string name;
        if (adapter->FriendlyName != nullptr) {
            const int count = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (count > 1) {
                name.resize(static_cast<std::size_t>(count - 1));
                WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, name.data(), count, nullptr, nullptr);
            }
        }
        const std::string mac = macText(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const sockaddr* addr = unicast->Address.lpSockaddr;
            Interface entry;
            entry.name = name;
            entry.mac = mac;
            entry.internal = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            entry.prefix = unicast->OnLinkPrefixLength;
            Address address;
            if (addr->sa_family == AF_INET) {
                address.version = 4;
                std::memcpy(address.bytes.data(), &reinterpret_cast<const sockaddr_in*>(addr)->sin_addr, 4);
                entry.family = "ipv4";
                Address mask;
                mask.version = 4;
                for (int i = 0; i < 4; ++i) {
                    const int bits = std::max(0, std::min(8, entry.prefix - i * 8));
                    mask.bytes[static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits == 0 ? 0 : 0xFF << (8 - bits));
                }
                entry.netmask = formatAddress(mask);
            } else if (addr->sa_family == AF_INET6) {
                address.version = 6;
                std::memcpy(address.bytes.data(), &reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr, 16);
                entry.family = "ipv6";
            } else {
                continue;
            }
            entry.address = formatAddress(address);
            out.push_back(std::move(entry));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return out;
    }
    std::vector<std::pair<std::string, std::string>> macs;
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr) {
            continue;
        }
#if defined(__linux__)
        if (entry->ifa_addr->sa_family == AF_PACKET) {
            const auto* link = reinterpret_cast<const sockaddr_ll*>(entry->ifa_addr);
            macs.emplace_back(entry->ifa_name, macText(link->sll_addr, link->sll_halen));
        }
#elif defined(__APPLE__) || defined(__FreeBSD__)
        if (entry->ifa_addr->sa_family == AF_LINK) {
            const auto* link = reinterpret_cast<const sockaddr_dl*>(entry->ifa_addr);
            macs.emplace_back(entry->ifa_name, macText(reinterpret_cast<const unsigned char*>(LLADDR(link)), link->sdl_alen));
        }
#endif
    }
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || (entry->ifa_addr->sa_family != AF_INET && entry->ifa_addr->sa_family != AF_INET6)) {
            continue;
        }
        Interface item;
        item.name = entry->ifa_name;
        item.internal = (entry->ifa_flags & IFF_LOOPBACK) != 0;
        Address address;
        Address mask;
        if (entry->ifa_addr->sa_family == AF_INET) {
            address.version = mask.version = 4;
            item.family = "ipv4";
            std::memcpy(address.bytes.data(), &reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr, 4);
            if (entry->ifa_netmask != nullptr) {
                std::memcpy(mask.bytes.data(), &reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr, 4);
            }
        } else {
            address.version = mask.version = 6;
            item.family = "ipv6";
            const auto* v6 = reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr);
            std::memcpy(address.bytes.data(), &v6->sin6_addr, 16);
            if (v6->sin6_scope_id != 0) {
                address.zone = entry->ifa_name;
            }
            if (entry->ifa_netmask != nullptr) {
                std::memcpy(mask.bytes.data(), &reinterpret_cast<const sockaddr_in6*>(entry->ifa_netmask)->sin6_addr, 16);
            }
        }
        item.address = formatAddress(address);
        item.prefix = prefixLength(mask.bytes.data(), mask.size());
        mask.zone.clear();
        item.netmask = formatAddress(mask);
        for (const auto& [name, mac] : macs) {
            if (name == item.name) {
                item.mac = mac;
            }
        }
        out.push_back(std::move(item));
    }
    freeifaddrs(list);
#endif
    return out;
}

// net.interfaces(): { { name, address, family, netmask, prefix, internal, mac? } }
int netInterfaces(lua_State* L) {
    const std::vector<Interface> interfaces = listInterfaces();
    lua_createtable(L, static_cast<int>(interfaces.size()), 0);
    for (std::size_t i = 0; i < interfaces.size(); ++i) {
        const Interface& item = interfaces[i];
        lua_createtable(L, 0, 7);
        setStringField(L, "name", item.name);
        setStringField(L, "address", item.address);
        setStringField(L, "family", item.family);
        if (!item.netmask.empty()) {
            setStringField(L, "netmask", item.netmask);
        }
        setNumberField(L, "prefix", item.prefix);
        setBoolField(L, "internal", item.internal);
        if (!item.mac.empty() && item.mac != "00:00:00:00:00:00") {
            setStringField(L, "mac", item.mac);
        }
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

const Function kIpFunctions[] = {
    {"parse", ipParse},         {"version", ipVersion},   {"isValid", ipIsValid}, {"normalize", ipNormalize},
    {"fromBytes", ipFromBytes}, {"kind", ipKind},         {"isPublic", ipIsPublic}, {"inRange", ipInRange},
    {"parseCidr", ipParseCidr},
};

} // namespace

void pushIp(lua_State* L) {
    ensureNetworking();
    lua_createtable(L, 0, static_cast<int>(std::size(kIpFunctions)));
    setFunctionList(L, kIpFunctions);
}

void pushInterfaces(lua_State* L) {
    lua_pushcfunction(L, netInterfaces, "interfaces");
    lua_setfield(L, -2, "interfaces");
}

} // namespace sonata::lib::libs::net
