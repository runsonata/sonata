// @sonata/net DNS: the system resolver (getaddrinfo / getnameinfo) and a small DNS
// client for the record types the system resolver cannot look up (MX, TXT, SRV, ...).
// Every lookup runs on a scheduler worker, so it never blocks other tasks.

#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
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
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <sonata/core/scheduler.hpp>
#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs::net {

namespace {

using sched::Scheduler;

#ifdef _WIN32
using SockT = SOCKET;
constexpr SockT kInvalid = INVALID_SOCKET;
void closeSock(SockT fd) { closesocket(fd); }
int pollOne(SockT fd, short events, int ms) {
    WSAPOLLFD p{};
    p.fd = fd;
    p.events = events;
    return WSAPoll(&p, 1, ms);
}
#else
using SockT = int;
constexpr SockT kInvalid = -1;
void closeSock(SockT fd) { ::close(fd); }
int pollOne(SockT fd, short events, int ms) {
    pollfd p{fd, events, 0};
    int rc = 0;
    do {
        rc = ::poll(&p, 1, ms);
    } while (rc < 0 && errno == EINTR);
    return rc;
}
#endif

std::string gaiError(int rc) {
#ifdef _WIN32
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(rc), 0, buf, sizeof buf, nullptr);
    std::string text = buf;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
#else
    return gai_strerror(rc);
#endif
}

std::string numericHost(const sockaddr* addr, socklen_t length) {
    char host[NI_MAXHOST] = {};
    if (getnameinfo(addr, length, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0) {
        return {};
    }
    return host;
}

int familyArg(lua_State* L, int idx) {
    static const char* const kFamilies[] = {"any", "ipv4", "ipv6", nullptr};
    static const int kValues[] = {AF_UNSPEC, AF_INET, AF_INET6};
    return kValues[luaL_checkoption(L, idx, "any", kFamilies)];
}

// Pushes a list of strings, or nil + error.
sched::Completion listResult(std::vector<std::string> items, std::string error) {
    return [items = std::move(items), error = std::move(error)](lua_State* T) {
        if (!error.empty()) {
            return failure(T, error);
        }
        lua_createtable(T, static_cast<int>(items.size()), 0);
        for (std::size_t i = 0; i < items.size(); ++i) {
            pushString(T, items[i]);
            lua_rawseti(T, -2, static_cast<int>(i + 1));
        }
        return 1;
    };
}

// net.dns.resolve(host: string, family: string?): { string } | (nil, string)
int dnsResolve(lua_State* L) {
    const std::string host = luaL_checkstring(L, 1);
    const int family = familyArg(L, 2);
    ensureNetworking();

    return Scheduler::of(L).await(L, [host, family]() -> sched::Completion {
        addrinfo hints{};
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &list);
        if (rc != 0) {
            return listResult({}, gaiError(rc));
        }
        std::vector<std::string> addresses;
        for (const addrinfo* a = list; a != nullptr; a = a->ai_next) {
            std::string text = numericHost(a->ai_addr, static_cast<socklen_t>(a->ai_addrlen));
            if (!text.empty() && std::find(addresses.begin(), addresses.end(), text) == addresses.end()) {
                addresses.push_back(std::move(text));
            }
        }
        freeaddrinfo(list);
        return listResult(std::move(addresses), {});
    });
}

// net.dns.reverse(address: string): string | (nil, string)
int dnsReverse(lua_State* L) {
    const std::string address = luaL_checkstring(L, 1);
    ensureNetworking();

    return Scheduler::of(L).await(L, [address]() -> sched::Completion {
        sockaddr_storage storage{};
        socklen_t length = 0;
        auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
        std::string error;
        std::string name;
        if (inet_pton(AF_INET, address.c_str(), &v4->sin_addr) == 1) {
            v4->sin_family = AF_INET;
            length = sizeof(sockaddr_in);
        } else if (inet_pton(AF_INET6, address.c_str(), &v6->sin6_addr) == 1) {
            v6->sin6_family = AF_INET6;
            length = sizeof(sockaddr_in6);
        } else {
            error = "not an IP address";
        }
        if (error.empty()) {
            char host[NI_MAXHOST] = {};
            const int rc = getnameinfo(reinterpret_cast<sockaddr*>(&storage), length, host, sizeof host, nullptr, 0, NI_NAMEREQD);
            if (rc != 0) {
                error = gaiError(rc);
            } else {
                name = host;
            }
        }
        return [name, error](lua_State* T) {
            if (!error.empty()) {
                return failure(T, error);
            }
            pushString(T, name);
            return 1;
        };
    });
}

// The system's DNS servers.
std::vector<std::string> systemServers() {
    std::vector<std::string> servers;
#ifdef _WIN32
    ULONG size = 0;
    GetNetworkParams(nullptr, &size);
    std::vector<unsigned char> buffer(size);
    auto* info = reinterpret_cast<FIXED_INFO*>(buffer.data());
    if (size != 0 && GetNetworkParams(info, &size) == NO_ERROR) {
        for (const IP_ADDR_STRING* entry = &info->DnsServerList; entry != nullptr; entry = entry->Next) {
            if (entry->IpAddress.String[0] != '\0') {
                servers.emplace_back(entry->IpAddress.String);
            }
        }
    }
#else
    std::ifstream file("/etc/resolv.conf");
    std::string line;
    while (std::getline(file, line)) {
        char server[256] = {};
        if (std::sscanf(line.c_str(), " nameserver %255s", server) == 1) {
            std::string text = server;
            if (const std::size_t zone = text.find('%'); zone != std::string::npos) {
                text.erase(zone); // link-local zones are not worth the trouble here
            }
            servers.push_back(text);
        }
    }
#endif
    return servers;
}

// net.dns.servers(): { string }
int dnsServers(lua_State* L) {
    const std::vector<std::string> servers = systemServers();
    lua_createtable(L, static_cast<int>(servers.size()), 0);
    for (std::size_t i = 0; i < servers.size(); ++i) {
        pushString(L, servers[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// The DNS client ////////////////////////////////////////////////////////////////

struct RecordType {
    const char* name;
    int code;
};

constexpr RecordType kTypes[] = {
    {"A", 1},     {"NS", 2},    {"CNAME", 5},  {"SOA", 6},   {"PTR", 12},   {"MX", 15},  {"TXT", 16},
    {"AAAA", 28}, {"SRV", 33},  {"NAPTR", 35}, {"DS", 43},   {"DNSKEY", 48}, {"HTTPS", 65}, {"SVCB", 64},
    {"CAA", 257}, {"ANY", 255},
};

const char* typeName(int code) {
    for (const RecordType& type : kTypes) {
        if (type.code == code) {
            return type.name;
        }
    }
    return nullptr;
}

struct Record {
    std::string name;
    int type = 0;
    std::uint32_t ttl = 0;
    // decoded fields (the ones that apply to the type)
    std::string address, target, text, exchange, tag, value, mname, rname;
    std::vector<std::string> strings;
    int priority = 0, weight = 0, port = 0, flags = 0;
    std::uint32_t serial = 0, refresh = 0, retry = 0, expire = 0, minimum = 0;
    std::string data; // raw rdata for types we do not decode
};

struct Answer {
    std::vector<Record> records;
    std::string error;
};

void put16(std::string& out, int value) {
    out += static_cast<char>((value >> 8) & 0xFF);
    out += static_cast<char>(value & 0xFF);
}

bool encodeName(std::string& out, const std::string& name) {
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t dot = name.find('.', start);
        if (dot == std::string::npos) {
            dot = name.size();
        }
        const std::size_t length = dot - start;
        if (length == 0 || length > 63) {
            return false;
        }
        out += static_cast<char>(length);
        out.append(name, start, length);
        start = dot + 1;
    }
    out += '\0';
    return out.size() <= 512;
}

std::string buildQuery(std::uint16_t id, const std::string& name, int type) {
    std::string query;
    put16(query, id);
    put16(query, 0x0100); // recursion desired
    put16(query, 1);      // one question
    put16(query, 0);
    put16(query, 0);
    put16(query, 1);      // one additional record: EDNS0
    if (!encodeName(query, name)) {
        return {};
    }
    put16(query, type);
    put16(query, 1);      // class IN
    query += '\0';        // OPT: root name
    put16(query, 41);     // type OPT
    put16(query, 4096);   // our UDP payload size
    put16(query, 0);
    put16(query, 0);      // extended rcode, version, flags
    put16(query, 0);      // no data
    return query;
}

class Reader {
  public:
    explicit Reader(const std::string& message) : message_(message) {}

    bool u8(int& out) {
        if (pos_ + 1 > message_.size()) return false;
        out = static_cast<unsigned char>(message_[pos_++]);
        return true;
    }
    bool u16(int& out) {
        if (pos_ + 2 > message_.size()) return false;
        out = (static_cast<unsigned char>(message_[pos_]) << 8) | static_cast<unsigned char>(message_[pos_ + 1]);
        pos_ += 2;
        return true;
    }
    bool u32(std::uint32_t& out) {
        int hi = 0;
        int lo = 0;
        if (!u16(hi) || !u16(lo)) return false;
        out = (static_cast<std::uint32_t>(hi) << 16) | static_cast<std::uint32_t>(lo);
        return true;
    }
    bool bytes(std::size_t count, std::string& out) {
        if (pos_ + count > message_.size()) return false;
        out.assign(message_, pos_, count);
        pos_ += count;
        return true;
    }
    // A (possibly compressed) domain name.
    bool name(std::string& out) {
        out.clear();
        std::size_t at = pos_;
        bool jumped = false;
        for (int hops = 0; hops < 64; ++hops) {
            if (at >= message_.size()) return false;
            const auto length = static_cast<unsigned char>(message_[at]);
            if (length == 0) {
                if (!jumped) pos_ = at + 1;
                if (out.empty()) out = ".";
                return true;
            }
            if ((length & 0xC0) == 0xC0) {
                if (at + 1 >= message_.size()) return false;
                const std::size_t target = ((length & 0x3F) << 8) | static_cast<unsigned char>(message_[at + 1]);
                if (!jumped) pos_ = at + 2;
                jumped = true;
                at = target;
                continue;
            }
            if (at + 1 + length > message_.size()) return false;
            if (!out.empty()) out += '.';
            out.append(message_, at + 1, length);
            at += 1 + length;
        }
        return false;
    }
    std::size_t pos() const { return pos_; }
    void seek(std::size_t pos) { pos_ = pos; }

  private:
    const std::string& message_;
    std::size_t pos_ = 0;
};

const char* rcodeText(int rcode) {
    switch (rcode) {
    case 1: return "the server could not understand the query";
    case 2: return "server failure";
    case 3: return "no such domain";
    case 4: return "the server does not support this query";
    case 5: return "the server refused the query";
    default: return "DNS error";
    }
}

bool parseRecord(Reader& r, Record& record) {
    int type = 0;
    int klass = 0;
    int length = 0;
    if (!r.name(record.name) || !r.u16(type) || !r.u16(klass) || !r.u32(record.ttl) || !r.u16(length)) {
        return false;
    }
    record.type = type;
    const std::size_t end = r.pos() + static_cast<std::size_t>(length);

    switch (type) {
    case 1:
    case 28: {
        std::string raw;
        if (!r.bytes(static_cast<std::size_t>(length), raw) || (raw.size() != 4 && raw.size() != 16)) return false;
        char text[INET6_ADDRSTRLEN] = {};
        inet_ntop(raw.size() == 4 ? AF_INET : AF_INET6, raw.data(), text, sizeof text);
        record.address = text;
        break;
    }
    case 2:
    case 5:
    case 12:
        if (!r.name(record.target)) return false;
        break;
    case 15:
        if (!r.u16(record.priority) || !r.name(record.exchange)) return false;
        break;
    case 16:
        while (r.pos() < end) {
            int size = 0;
            std::string piece;
            if (!r.u8(size) || !r.bytes(static_cast<std::size_t>(size), piece)) return false;
            record.strings.push_back(piece);
            record.text += piece;
        }
        break;
    case 33:
        if (!r.u16(record.priority) || !r.u16(record.weight) || !r.u16(record.port) || !r.name(record.target)) return false;
        break;
    case 6:
        if (!r.name(record.mname) || !r.name(record.rname) || !r.u32(record.serial) || !r.u32(record.refresh) ||
            !r.u32(record.retry) || !r.u32(record.expire) || !r.u32(record.minimum)) {
            return false;
        }
        break;
    case 257: {
        int tagLength = 0;
        if (!r.u8(record.flags) || !r.u8(tagLength) || !r.bytes(static_cast<std::size_t>(tagLength), record.tag)) return false;
        if (r.pos() > end || !r.bytes(end - r.pos(), record.value)) return false;
        break;
    }
    default:
        if (!r.bytes(static_cast<std::size_t>(length), record.data)) return false;
        break;
    }
    r.seek(end);
    return true;
}

Answer parseResponse(const std::string& message, std::uint16_t id, bool& truncated) {
    Answer answer;
    Reader r(message);
    int gotId = 0, flags = 0, questions = 0, answers = 0, authority = 0, additional = 0;
    if (!r.u16(gotId) || !r.u16(flags) || !r.u16(questions) || !r.u16(answers) || !r.u16(authority) || !r.u16(additional) ||
        gotId != id) {
        answer.error = "malformed DNS response";
        return answer;
    }
    truncated = (flags & 0x0200) != 0;
    const int rcode = flags & 0xF;
    if (rcode != 0) {
        answer.error = rcodeText(rcode);
        return answer;
    }
    for (int i = 0; i < questions; ++i) {
        std::string name;
        int ignored = 0;
        if (!r.name(name) || !r.u16(ignored) || !r.u16(ignored)) {
            answer.error = "malformed DNS response";
            return answer;
        }
    }
    for (int i = 0; i < answers; ++i) {
        Record record;
        if (!parseRecord(r, record)) {
            answer.error = "malformed DNS response";
            return answer;
        }
        answer.records.push_back(std::move(record));
    }
    return answer;
}

bool serverAddress(const std::string& server, int defaultPort, sockaddr_storage& out, socklen_t& length) {
    std::string host = server;
    int port = defaultPort;
    if (!host.empty() && host[0] == '[') {
        const std::size_t close = host.find(']');
        if (close == std::string::npos) return false;
        if (close + 2 < host.size() && host[close + 1] == ':') port = std::atoi(host.c_str() + close + 2);
        host = host.substr(1, close - 1);
    } else if (std::count(host.begin(), host.end(), ':') == 1) {
        const std::size_t colon = host.find(':');
        port = std::atoi(host.c_str() + colon + 1);
        host.erase(colon);
    }
    out = sockaddr_storage{};
    auto* v4 = reinterpret_cast<sockaddr_in*>(&out);
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&out);
    if (inet_pton(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(static_cast<unsigned short>(port));
        length = sizeof(sockaddr_in);
        return true;
    }
    if (inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(static_cast<unsigned short>(port));
        length = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

int msLeft(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

// One exchange over UDP. Returns the response, or "" (error set).
std::string exchangeUdp(const sockaddr_storage& addr, socklen_t length, const std::string& query,
                        std::chrono::steady_clock::time_point deadline, std::string& error) {
    const SockT fd = ::socket(addr.ss_family, SOCK_DGRAM, 0);
    if (fd == kInvalid) {
        error = "cannot create a socket";
        return {};
    }
    std::string response;
    if (::sendto(fd, query.data(), static_cast<int>(query.size()), 0, reinterpret_cast<const sockaddr*>(&addr), length) < 0) {
        error = "cannot reach the DNS server";
    } else if (pollOne(fd, POLLIN, msLeft(deadline)) <= 0) {
        error = "timed out";
    } else {
        char buffer[4096];
        const auto got = ::recv(fd, buffer, sizeof buffer, 0);
        if (got <= 0) {
            error = "no answer from the DNS server";
        } else {
            response.assign(buffer, static_cast<std::size_t>(got));
        }
    }
    closeSock(fd);
    return response;
}

// One exchange over TCP (for answers too big for UDP).
std::string exchangeTcp(const sockaddr_storage& addr, socklen_t length, const std::string& query,
                        std::chrono::steady_clock::time_point deadline, std::string& error) {
    const SockT fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
    if (fd == kInvalid) {
        error = "cannot create a socket";
        return {};
    }
    std::string response;
    std::string framed;
    put16(framed, static_cast<int>(query.size()));
    framed += query;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), length) != 0 ||
        ::send(fd, framed.data(), static_cast<int>(framed.size()), 0) != static_cast<long long>(framed.size())) {
        error = "cannot reach the DNS server over TCP";
    } else {
        std::string received;
        char buffer[4096];
        for (;;) {
            if (received.size() >= 2) {
                const std::size_t want = (static_cast<unsigned char>(received[0]) << 8) | static_cast<unsigned char>(received[1]);
                if (received.size() >= want + 2) {
                    response = received.substr(2, want);
                    break;
                }
            }
            if (pollOne(fd, POLLIN, msLeft(deadline)) <= 0) {
                error = "timed out";
                break;
            }
            const auto got = ::recv(fd, buffer, sizeof buffer, 0);
            if (got <= 0) {
                error = "the DNS server closed the connection";
                break;
            }
            received.append(buffer, static_cast<std::size_t>(got));
        }
    }
    closeSock(fd);
    return response;
}

Answer query(const std::string& name, int type, std::vector<std::string> servers, double timeout, bool tcpOnly) {
    Answer answer;
    if (servers.empty()) {
        servers = systemServers();
    }
    if (servers.empty()) {
        answer.error = "no DNS server is configured";
        return answer;
    }

    const std::string bytes = randomBytes(2);
    const auto id = static_cast<std::uint16_t>((static_cast<unsigned char>(bytes[0]) << 8) | static_cast<unsigned char>(bytes[1]));
    const std::string message = buildQuery(id, name, type);
    if (message.empty()) {
        answer.error = "invalid domain name";
        return answer;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout));
    for (const std::string& server : servers) {
        sockaddr_storage addr{};
        socklen_t length = 0;
        if (!serverAddress(server, 53, addr, length)) {
            answer.error = "invalid DNS server address '" + server + "'";
            continue;
        }
        std::string error;
        bool truncated = false;
        std::string response = tcpOnly ? exchangeTcp(addr, length, message, deadline, error)
                                       : exchangeUdp(addr, length, message, deadline, error);
        if (!response.empty()) {
            answer = parseResponse(response, id, truncated);
            if (truncated && !tcpOnly) {
                response = exchangeTcp(addr, length, message, deadline, error);
                if (!response.empty()) {
                    answer = parseResponse(response, id, truncated);
                }
            }
            if (answer.error.empty() || answer.error == "no such domain") {
                return answer;
            }
        } else {
            answer.error = error;
        }
        if (msLeft(deadline) == 0) {
            break;
        }
    }
    return answer;
}

void pushRecord(lua_State* L, const Record& record) {
    lua_createtable(L, 0, 6);
    setStringField(L, "name", record.name);
    const char* type = typeName(record.type);
    if (type != nullptr) {
        setStringField(L, "type", type);
    } else {
        setNumberField(L, "type", record.type);
    }
    setNumberField(L, "ttl", record.ttl);
    switch (record.type) {
    case 1:
    case 28:
        setStringField(L, "address", record.address);
        break;
    case 2:
    case 5:
    case 12:
        setStringField(L, "target", record.target);
        break;
    case 15:
        setNumberField(L, "priority", record.priority);
        setStringField(L, "exchange", record.exchange);
        break;
    case 16:
        setStringField(L, "text", record.text);
        lua_createtable(L, static_cast<int>(record.strings.size()), 0);
        for (std::size_t i = 0; i < record.strings.size(); ++i) {
            pushString(L, record.strings[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "strings");
        break;
    case 33:
        setNumberField(L, "priority", record.priority);
        setNumberField(L, "weight", record.weight);
        setNumberField(L, "port", record.port);
        setStringField(L, "target", record.target);
        break;
    case 6:
        setStringField(L, "mname", record.mname);
        setStringField(L, "rname", record.rname);
        setNumberField(L, "serial", record.serial);
        setNumberField(L, "refresh", record.refresh);
        setNumberField(L, "retry", record.retry);
        setNumberField(L, "expire", record.expire);
        setNumberField(L, "minimum", record.minimum);
        break;
    case 257:
        setNumberField(L, "flags", record.flags);
        setStringField(L, "tag", record.tag);
        setStringField(L, "value", record.value);
        break;
    default:
        setStringField(L, "data", record.data);
        break;
    }
}

// net.dns.query(name: string, type: string?, options: { server: string | { string }?, timeout: number?, tcp: boolean? }?):
//     { Record } | (nil, string)
int dnsQuery(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    while (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    int type = 1;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        type = luaL_checkinteger(L, 2);
        luaL_argcheck(L, type > 0 && type < 65536, 2, "record type out of range");
    } else if (!lua_isnoneornil(L, 2)) {
        const std::string wanted = lowercase(luaL_checkstring(L, 2));
        type = 0;
        for (const RecordType& t : kTypes) {
            if (lowercase(t.name) == wanted) {
                type = t.code;
            }
        }
        luaL_argcheck(L, type != 0, 2, "unknown record type");
    }

    std::vector<std::string> servers;
    double timeout = 5.0;
    bool tcp = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "server");
        if (lua_type(L, -1) == LUA_TSTRING) {
            servers.emplace_back(lua_tostring(L, -1));
        } else if (lua_istable(L, -1)) {
            const int n = static_cast<int>(lua_objlen(L, -1));
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_type(L, -1) == LUA_TSTRING) {
                    servers.emplace_back(lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
        }
        lua_getfield(L, 3, "timeout");
        if (!lua_isnil(L, -1)) {
            timeout = luaL_checknumber(L, -1);
            luaL_argcheck(L, timeout > 0, 3, "timeout must be positive");
        }
        lua_getfield(L, 3, "tcp");
        tcp = lua_toboolean(L, -1) != 0;
    }
    ensureNetworking();

    return Scheduler::of(L).await(L, [name, type, servers, timeout, tcp]() -> sched::Completion {
        auto answer = std::make_shared<Answer>(query(name, type, servers, timeout, tcp));
        return [answer](lua_State* T) {
            if (!answer->error.empty()) {
                return failure(T, answer->error);
            }
            lua_createtable(T, static_cast<int>(answer->records.size()), 0);
            for (std::size_t i = 0; i < answer->records.size(); ++i) {
                pushRecord(T, answer->records[i]);
                lua_rawseti(T, -2, static_cast<int>(i + 1));
            }
            return 1;
        };
    });
}

const Function kDnsFunctions[] = {
    {"resolve", dnsResolve},
    {"reverse", dnsReverse},
    {"query", dnsQuery},
    {"servers", dnsServers},
};

} // namespace

void pushDns(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kDnsFunctions)));
    setFunctionList(L, kDnsFunctions);
}

} // namespace sonata::lib::libs::net
