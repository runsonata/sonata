// @sonata/net: TCP/UDP sockets, DNS lookups and URL helpers.
//
//     local net = require("@sonata/net")
//
// Failures that depend on the outside world (refused, timed out, DNS...) do NOT
// raise: the call returns nil, "message". Only misuse (wrong argument types,
// using a closed socket) raises. All calls block the calling thread, for at
// most the socket's timeout. Sockets close themselves when garbage collected.
//
//     net.connect(host, port, timeout?)        --> Socket   TCP client
//     net.listen(host?, port, backlog?)        --> Socket   TCP server (host nil = all interfaces, port 0 = any)
//     net.udp(host?, port?, reuse?)            --> Socket   UDP (bound if host or port is given; reuse lets several sockets share the port)
//     net.raw(protocol, family?)               --> Socket   raw IP socket: "icmp" | "icmpv6" | "tcp" | "udp" | 0-255, family "ipv4" | "ipv6"
//                                                           (needs root/administrator, or CAP_NET_RAW on Linux)
//     net.resolve(host, family?)               --> { "1.2.3.4", "::1" }   family: "any" | "ipv4" | "ipv6"
//     net.select(readers, writers?, timeout?)  --> { ready to read }, { ready to write }
//
//     sock:send(data)              --> bytesSent     sends everything, or nil, err, bytesSentSoFar
//     sock:recv(max?, exact?)      --> string        up to max bytes (default 4096); exact waits for all of them
//                                                    "" means the peer closed the connection
//     sock:recvUntil(delim, max?)  --> string        includes the delimiter; max defaults to 1 MiB
//     sock:accept()                --> Socket        (server sockets)
//     sock:sendTo(data, host, port?)--> bytesSent    (UDP, or raw: no port)
//     sock:recvFrom(max?)          --> data, host, port   (UDP/raw, max defaults to 2048; IPv4 raw data starts with the IP header)
//     sock:startTls(options?)      --> true          upgrade a TCP socket to TLS (see below)
//     sock:tlsInfo()               --> { version, cipher, alpn? } | nil
//     sock:joinGroup(group, iface?), sock:leaveGroup(group, iface?)    multicast membership (UDP; iface = local IPv4 address,
//                                                    or IPv6 interface name/index; the socket's family must match the group)
//     sock:setMulticastInterface(iface)              outgoing multicast interface
//     sock:shutdown(how?)          --> true          "write" (default) | "read" | "both"
//     sock:setTimeout(seconds?)    nil = wait forever (default), 0 = never wait; applies to every wait
//     sock:setOption(name, value)  booleans: "nodelay" | "keepalive" | "broadcast" | "hdrincl" (IPv4 raw) | "multicastLoop"; number: "multicastTtl"
//     sock:localAddress(), sock:peerAddress()  --> host, port
//     sock:close()
//
//     net.url.parse("https://u:p@Host:8443/a/b?x=1#top")
//         --> { scheme="https", username="u", password="p", host="host", port=8443,
//               path="/a/b", query="x=1", fragment="top" }     (absent parts are nil; not decoded)
//     net.url.build({ scheme="https", host="a.com", path="/s", query={ q="a b" } })  --> "https://a.com/s?q=a%20b"
//     net.url.encode("a b/c", "/")                 --> "a%20b/c"       second arg: extra characters to keep
//     net.url.decode("a%20b+c", true)              --> "a b c"         true: '+' means space
//     net.url.parseQuery("a=1&b=2&a=3")            --> { a="1", b="2" }, { {"a","1"}, {"b","2"}, {"a","3"} }
//     net.url.buildQuery({ {"a","1"}, {"a","2"} }) --> "a=1&a=2"       (a { k=v } map works too, order unspecified)
//
// Plain HTTP/1.1 is a few lines on top of the primitives (for HTTPS call s:startTls() first, see below):
//
//     local u = net.url.parse("http://example.com/")
//     local s = assert(net.connect(u.host, u.port or 80, 10))
//     s:send("GET / HTTP/1.1\r\nHost: " .. u.host .. "\r\nConnection: close\r\n\r\n")
//     local head = assert(s:recvUntil("\r\n\r\n"))
//     local parts = {}
//     repeat local chunk = assert(s:recv(65536)); table.insert(parts, chunk) until chunk == ""
//
// TLS (OpenSSL; compiled in only with -DSONATA_NET_TLS, link with -lssl -lcrypto):
//
//     local s = assert(net.connect("example.com", 443, 10))
//     assert(s:startTls())    -- verifies certificate chain and host name against the system trust store
//
//   options: host (name for SNI and verification, default: the connect() host), verify (default true),
//            caFile (PEM bundle instead of the system store), alpn ({ "h2", "http/1.1" }, see s:tlsInfo().alpn)
//   Server: on a socket returned by accept(), s:startTls({ cert = "cert.pem", key = "key.pem" })
//   Mid-stream upgrades (SMTP/IMAP STARTTLS) work, but startTls refuses if unread data is buffered.
//   TLS 1.2 or newer only. verify = false disables ALL certificate checks: use it for testing only.
//
//   1. Declare "void openNet(lua_State* L);" in libs.hpp.
//   2. Register it under "@sonata/net" next to the other built-ins.
//   3. On Windows, link ws2_32. For TLS, define SONATA_NET_TLS and link OpenSSL.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#ifdef SONATA_NET_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

// Rule of thumb for every binding (same as example.cpp): read and validate ALL
// arguments first, and only then create C++ objects. The bindings below are thin
// wrappers around plain C++ functions that never call into Lua, so errors are
// reported as values and no destructor is ever skipped.

// Platform layer //////////////////////////////////////////////7

#ifdef _WIN32
using SockT = SOCKET;
using PollFd = WSAPOLLFD;
using SockLen = int;
constexpr SockT kInvalid = INVALID_SOCKET;

int lastError() { return WSAGetLastError(); }
void closeSocket(SockT fd) { closesocket(fd); }
int pollSockets(PollFd* fds, std::size_t n, int ms) { return WSAPoll(fds, static_cast<ULONG>(n), ms); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
bool inProgress(int e) { return e == WSAEWOULDBLOCK; }
bool interrupted(int) { return false; }

std::string errorText(int code) {
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(code), 0, buf,
                   sizeof buf, nullptr);
    std::string text = buf;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text.empty() ? "socket error " + std::to_string(code) : text;
}

std::string gaiText(int rc) { return errorText(rc); }

long long sockRecv(SockT fd, char* buf, std::size_t n) { return ::recv(fd, buf, static_cast<int>(std::min<std::size_t>(n, 1 << 30)), 0); }
long long sockSend(SockT fd, const char* buf, std::size_t n) {
    return ::send(fd, buf, static_cast<int>(std::min<std::size_t>(n, 1 << 30)), 0);
}

void prepare(SockT fd) {
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
}
#else
using SockT = int;
using PollFd = pollfd;
using SockLen = socklen_t;
constexpr SockT kInvalid = -1;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int lastError() { return errno; }
void closeSocket(SockT fd) { ::close(fd); }
int pollSockets(PollFd* fds, std::size_t n, int ms) { return ::poll(fds, static_cast<nfds_t>(n), ms); }
bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
bool inProgress(int e) { return e == EINPROGRESS || e == EINTR; }
bool interrupted(int e) { return e == EINTR; }
std::string errorText(int code) { return std::strerror(code); }
std::string gaiText(int rc) { return gai_strerror(rc); }

long long sockRecv(SockT fd, char* buf, std::size_t n) { return ::recv(fd, buf, n, 0); }
long long sockSend(SockT fd, const char* buf, std::size_t n) { return ::send(fd, buf, n, MSG_NOSIGNAL); }

// Every socket is non-blocking: timeouts are implemented with poll() in runIO().
void prepare(SockT fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
#endif

void ensureNetworking() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

bool setFlag(SockT fd, int level, int name, bool on) {
    const int value = on ? 1 : 0;
    return ::setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof value) == 0;
}

std::string rawError(int code) {
    std::string text = errorText(code);
#ifdef _WIN32
    const bool denied = code == WSAEACCES;
#else
    const bool denied = code == EPERM || code == EACCES;
#endif
    return denied ? text + " (raw/ICMP sockets need root or administrator rights, or CAP_NET_RAW on Linux)" : text;
}

// Sockets (plain C++, no Lua calls) //////////////////////////////////////////////

constexpr const char* kSocketMeta = "sonata.net.Socket";
constexpr const char* kTimedOut = "timed out";
constexpr const char* kClosed = "closed";
constexpr std::size_t kMaxBuffer = 64u * 1024 * 1024;
#ifdef _WIN32
constexpr bool kReuseListen = false; // on Windows SO_REUSEADDR would let other processes steal the port
#else
constexpr bool kReuseListen = true; // a restarted server can rebind while old connections linger
#endif

enum class Kind { Tcp, Udp, Raw };

struct Socket {
    SockT fd = kInvalid;
    Kind kind = Kind::Tcp;
    int family = AF_INET;
    double timeout = -1;    // seconds; < 0 waits forever
    std::string pending;    // received from the OS but not yet consumed (TCP)
    std::string host;       // name given to connect(): default for TLS server-name checks
    std::string tlsError;   // set by the TLS layer when an operation fails for good
    bool accepted = false;  // came from accept(): startTls() then acts as the TLS server
    bool wantWrite = false; // which readiness runIO() waits for (the TLS layer may flip it)
#ifdef SONATA_NET_TLS
    SSL* ssl = nullptr;
#endif

    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    void close() {
#ifdef SONATA_NET_TLS
        if (ssl != nullptr) {
            SSL_shutdown(ssl); // best-effort close_notify; the socket is non-blocking
            SSL_free(ssl);
            ssl = nullptr;
            ERR_clear_error();
        }
#endif
        if (fd != kInvalid) {
            closeSocket(fd);
            fd = kInvalid;
        }
    }
};

enum class Wait { Ready, Timeout, Failed };

int toMillis(double seconds) {
    if (seconds < 0) {
        return -1;
    }
    return static_cast<int>(std::min(std::ceil(seconds * 1000.0), 2147483000.0));
}

Wait waitFor(SockT fd, bool forWrite, double timeout) {
    PollFd p{};
    p.fd = fd;
    p.events = forWrite ? POLLOUT : POLLIN;

    for (;;) {
        const int rc = pollSockets(&p, 1, toMillis(timeout));
        if (rc > 0) {
            return Wait::Ready;
        }
        if (rc == 0) {
            return Wait::Timeout;
        }
        if (!interrupted(lastError())) {
            return Wait::Failed;
        }
    }
}

// Calls "op" (returns -1 on failure) until it succeeds, waiting for the socket
// (up to its timeout) whenever the OS says it would block.
template <class Op>
long long runIO(Socket& s, bool forWrite, std::string& err, Op op) {
    for (;;) {
        s.wantWrite = forWrite; // the TLS layer flips this when a read has to write (or the reverse)
        const long long result = op();
        if (result >= 0) {
            return result;
        }

        const int code = lastError();
        if (!wouldBlock(code)) {
            err = s.tlsError.empty() ? errorText(code) : s.tlsError;
            s.tlsError.clear();
            return -1;
        }

        const Wait w = waitFor(s.fd, s.wantWrite, s.timeout);
        if (w == Wait::Timeout) {
            err = kTimedOut;
            return -1;
        }
        if (w == Wait::Failed) {
            err = errorText(lastError());
            return -1;
        }
    }
}

struct AddrFree {
    void operator()(addrinfo* a) const { freeaddrinfo(a); }
};
using AddrList = std::unique_ptr<addrinfo, AddrFree>;

// Blocking DNS lookup (getaddrinfo has no timeout). Returns null and sets err on failure.
AddrList lookup(const char* host, int port, int family, int socktype, int flags, std::string& err) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;

    char service[8];
    std::snprintf(service, sizeof service, "%d", port);

    addrinfo* raw = nullptr;
    const int rc = getaddrinfo(host, service, &hints, &raw);
    if (rc != 0) {
        err = gaiText(rc);
        return nullptr;
    }
    return AddrList(raw);
}

bool addressText(const sockaddr* addr, SockLen len, std::string& host, int& port) {
    char h[NI_MAXHOST];
    char p[NI_MAXSERV];
    if (getnameinfo(addr, len, h, sizeof h, p, sizeof p, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }
    host = h;
    port = std::atoi(p);
    return true;
}

// Finishes a non-blocking connect(), honouring the socket's timeout.
bool connectWait(Socket& s, const sockaddr* addr, SockLen len, std::string& err) {
    if (::connect(s.fd, addr, len) == 0) {
        return true;
    }

    const int code = lastError();
    if (!inProgress(code)) {
        err = errorText(code);
        return false;
    }

    const Wait w = waitFor(s.fd, true, s.timeout);
    if (w != Wait::Ready) {
        err = w == Wait::Timeout ? kTimedOut : errorText(lastError());
        return false;
    }

    int soError = 0;
    SockLen soLen = sizeof soError;
    getsockopt(s.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen);
    if (soError != 0) {
        err = errorText(soError);
        return false;
    }
    return true;
}

// Tries every address the name resolves to, in order.
bool openConnect(Socket& s, const char* host, int port, std::string& err) {
    const AddrList list = lookup(host, port, AF_UNSPEC, SOCK_STREAM, 0, err);
    if (!list) {
        return false;
    }
    s.host = host;

    err = "no usable address";
    for (const addrinfo* a = list.get(); a != nullptr; a = a->ai_next) {
        s.fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s.fd == kInvalid) {
            err = errorText(lastError());
            continue;
        }

        s.family = a->ai_family;
        prepare(s.fd);
        if (connectWait(s, a->ai_addr, static_cast<SockLen>(a->ai_addrlen), err)) {
            return true;
        }
        s.close();
    }
    return false;
}

// Binds s (and starts listening when backlog > 0).
bool bindOne(Socket& s, const char* host, int port, int family, int backlog, bool reuse, std::string& err) {
    const int socktype = s.kind == Kind::Tcp ? SOCK_STREAM : SOCK_DGRAM;
    const AddrList list = lookup(host, port, family, socktype, AI_PASSIVE, err);
    if (!list) {
        return false;
    }

    err = "no usable address";
    for (const addrinfo* a = list.get(); a != nullptr; a = a->ai_next) {
        s.fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s.fd == kInvalid) {
            err = errorText(lastError());
            continue;
        }

        s.family = a->ai_family;
        prepare(s.fd);
        if (reuse) {
            setFlag(s.fd, SOL_SOCKET, SO_REUSEADDR, true);
#ifdef SO_REUSEPORT
            if (s.kind == Kind::Udp) {
                setFlag(s.fd, SOL_SOCKET, SO_REUSEPORT, true); // BSD/macOS need this to share a multicast port
            }
#endif
        }
        if (a->ai_family == AF_INET6) {
            setFlag(s.fd, IPPROTO_IPV6, IPV6_V6ONLY, false); // "::" also accepts IPv4
        }

        if (::bind(s.fd, a->ai_addr, static_cast<SockLen>(a->ai_addrlen)) == 0 && (backlog == 0 || ::listen(s.fd, backlog) == 0)) {
            return true;
        }
        err = errorText(lastError());
        s.close();
    }
    return false;
}

// No host: wildcard address, IPv6 (dual-stack) first, IPv4 as the fallback.
bool openBound(Socket& s, const char* host, int port, int backlog, bool reuse, std::string& err) {
    if (host != nullptr) {
        return bindOne(s, host, port, AF_UNSPEC, backlog, reuse, err);
    }
    return bindOne(s, nullptr, port, AF_INET6, backlog, reuse, err) || bindOne(s, nullptr, port, AF_INET, backlog, reuse, err);
}

bool openUdp(Socket& s, const char* host, int port, bool reuse, std::string& err) {
    if (host == nullptr && port == 0) { // unbound, IPv4
        s.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s.fd == kInvalid) {
            err = errorText(lastError());
            return false;
        }
        s.family = AF_INET;
        prepare(s.fd);
        return true;
    }
    return openBound(s, host, port, 0, reuse, err);
}

bool openRaw(Socket& s, int family, int protocol, std::string& err) {
    s.fd = ::socket(family, SOCK_RAW, protocol);
    if (s.fd == kInvalid) {
        err = rawError(lastError());
        return false;
    }
    s.family = family;
    prepare(s.fd);
    return true;
}

// IPv6 interface: a numeric index, or (not on Windows) an interface name.
bool interfaceIndex(const char* name, unsigned& index, std::string& err) {
    char* end = nullptr;
    index = static_cast<unsigned>(std::strtoul(name, &end, 10));
    if (end != name && *end == '\0') {
        return true;
    }
#ifndef _WIN32
    index = if_nametoindex(name);
    if (index != 0) {
        return true;
    }
#endif
    err = "unknown interface";
    return false;
}

// IPv4: iface is a local address ("192.168.1.5"); IPv6: an interface index or name. nullptr = OS default.
bool changeGroup(Socket& s, const char* group, const char* iface, bool join, std::string& err) {
    const AddrList list = lookup(group, 0, AF_UNSPEC, SOCK_DGRAM, AI_NUMERICHOST, err);
    if (!list) {
        return false;
    }

    const addrinfo* a = list.get();
    if (a->ai_family != s.family) {
        err = "group address family does not match the socket (bind to \"0.0.0.0\" for IPv4 or \"::\" for IPv6)";
        return false;
    }

    int rc = 0;
    if (a->ai_family == AF_INET) {
        ip_mreq m{};
        m.imr_multiaddr = reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr;
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        if (iface != nullptr && inet_pton(AF_INET, iface, &m.imr_interface) != 1) {
            err = "interface must be a local IPv4 address";
            return false;
        }
        rc = ::setsockopt(s.fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m);
    } else {
        ipv6_mreq m{};
        m.ipv6mr_multiaddr = reinterpret_cast<const sockaddr_in6*>(a->ai_addr)->sin6_addr;
        unsigned index = 0;
        if (iface != nullptr && !interfaceIndex(iface, index, err)) {
            return false;
        }
        m.ipv6mr_interface = index;
        rc = ::setsockopt(s.fd, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, reinterpret_cast<const char*>(&m), sizeof m);
    }

    if (rc != 0) {
        err = errorText(lastError());
        return false;
    }
    return true;
}

enum class Mcast { Loop, Ttl, Interface };

// Loop: value 0/1. Ttl: hop limit 0-255. Interface: "iface" as for changeGroup().
bool setMulticast(Socket& s, Mcast what, int value, const char* iface, std::string& err) {
    int rc = 0;
    if (s.family == AF_INET) {
#ifdef _WIN32
        using Byte = int; // Windows wants an int here, the other systems an unsigned char
#else
        using Byte = unsigned char;
#endif
        const Byte byte = static_cast<Byte>(value);
        if (what == Mcast::Interface) {
            in_addr addr{};
            if (inet_pton(AF_INET, iface, &addr) != 1) {
                err = "interface must be a local IPv4 address";
                return false;
            }
            rc = ::setsockopt(s.fd, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&addr), sizeof addr);
        } else {
            rc = ::setsockopt(s.fd, IPPROTO_IP, what == Mcast::Loop ? IP_MULTICAST_LOOP : IP_MULTICAST_TTL,
                              reinterpret_cast<const char*>(&byte), sizeof byte);
        }
    } else if (what == Mcast::Interface) {
        unsigned index = 0;
        if (!interfaceIndex(iface, index, err)) {
            return false;
        }
        rc = ::setsockopt(s.fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, reinterpret_cast<const char*>(&index), sizeof index);
    } else {
        rc = ::setsockopt(s.fd, IPPROTO_IPV6, what == Mcast::Loop ? IPV6_MULTICAST_LOOP : IPV6_MULTICAST_HOPS,
                          reinterpret_cast<const char*>(&value), sizeof value);
    }

    if (rc != 0) {
        err = errorText(lastError());
        return false;
    }
    return true;
}

bool acceptOne(Socket& server, Socket& client, std::string& err) {
    SockT fd = kInvalid;
    const long long r = runIO(server, false, err, [&] {
        fd = ::accept(server.fd, nullptr, nullptr);
        return fd == kInvalid ? -1LL : 0LL;
    });
    if (r < 0) {
        return false;
    }

    prepare(fd);
    client.fd = fd;
    client.family = server.family;
    client.timeout = server.timeout;
    client.accepted = true;
    return true;
}

#ifdef SONATA_NET_TLS
// The TLS layer reports through runIO's errno convention: it sets errno to "would block" (and picks the
// direction to wait for) or to "failed" (with the message in Socket::tlsError).
void setWouldBlock() {
#ifdef _WIN32
    WSASetLastError(WSAEWOULDBLOCK);
#else
    errno = EAGAIN;
#endif
}

void setFailed() {
#ifdef _WIN32
    WSASetLastError(WSAECONNRESET);
#else
    errno = EPROTO;
#endif
}

std::string tlsErrorText(const Socket* s = nullptr) {
    if (s != nullptr && s->ssl != nullptr && SSL_get_verify_mode(s->ssl) != SSL_VERIFY_NONE) {
        const long verify = SSL_get_verify_result(s->ssl);
        if (verify != X509_V_OK) {
            ERR_clear_error();
            return std::string("certificate verification failed: ") + X509_verify_cert_error_string(verify);
        }
    }
    const unsigned long code = ERR_get_error();
    char buf[256] = {};
    if (code != 0) {
        ERR_error_string_n(code, buf, sizeof buf);
    }
    ERR_clear_error();
    return code != 0 ? std::string(buf) : std::string("TLS error");
}

// Converts an OpenSSL result: >= 0 is success (0 = clean EOF when "io"), -1 comes with errno set.
long long tlsResult(Socket& s, int r, bool io) {
    if (r > 0) {
        return r;
    }

    const int sys = lastError();
    switch (SSL_get_error(s.ssl, r)) {
    case SSL_ERROR_WANT_READ:
        s.wantWrite = false;
        setWouldBlock();
        return -1;
    case SSL_ERROR_WANT_WRITE:
        s.wantWrite = true;
        setWouldBlock();
        return -1;
    case SSL_ERROR_ZERO_RETURN:
        if (io) {
            return 0;
        }
        s.tlsError = "connection closed during the TLS handshake";
        break;
    case SSL_ERROR_SYSCALL:
        if (r == 0 && io) {
            return 0; // peer closed without close_notify
        }
        s.tlsError = r == 0 ? "unexpected EOF during the TLS handshake" : errorText(sys);
        ERR_clear_error();
        break;
    default:
        s.tlsError = tlsErrorText(&s);
        break;
    }
    setFailed();
    return -1;
}

bool tlsHandshake(Socket& s, bool server, std::string& err) {
    return runIO(s, false, err, [&] { return tlsResult(s, server ? SSL_accept(s.ssl) : SSL_connect(s.ssl), false); }) >= 0;
}

struct TlsOptions {
    const char* host = nullptr;
    const char* caFile = nullptr;
    const char* cert = nullptr;
    const char* key = nullptr;
    std::string_view alpn; // wire format: length-prefixed protocol names
    bool verify = true;
};

// Client handshake on connected sockets, server handshake on accepted ones.
bool startTls(Socket& s, const TlsOptions& o, std::string& err) {
    if (s.ssl != nullptr) {
        err = "TLS already started";
        return false;
    }
    if (!s.pending.empty()) {
        err = "unread data is buffered; read it before upgrading to TLS";
        return false;
    }

    const bool server = s.accepted;
    const char* host = o.host != nullptr ? o.host : s.host.c_str();
    if (server && o.cert == nullptr) {
        err = "TLS on an accepted socket needs a cert file";
        return false;
    }
    if (!server && o.cert != nullptr) {
        err = "cert is only valid on sockets returned by accept()";
        return false;
    }
    if (!server && o.verify && *host == '\0') {
        err = "host is required to verify the certificate (pass host, or verify = false)";
        return false;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (ctx == nullptr) {
        err = tlsErrorText();
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF); // many servers just close the TCP connection
#endif

    bool ok = true;
    if (server) {
        ok = SSL_CTX_use_certificate_chain_file(ctx, o.cert) == 1 &&
             SSL_CTX_use_PrivateKey_file(ctx, o.key != nullptr ? o.key : o.cert, SSL_FILETYPE_PEM) == 1 &&
             SSL_CTX_check_private_key(ctx) == 1;
    } else {
        if (o.verify) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            ok = o.caFile != nullptr ? SSL_CTX_load_verify_locations(ctx, o.caFile, nullptr) == 1
                                     : SSL_CTX_set_default_verify_paths(ctx) == 1;
        }
        if (ok && !o.alpn.empty()) {
            ok = SSL_CTX_set_alpn_protos(ctx, reinterpret_cast<const unsigned char*>(o.alpn.data()),
                                         static_cast<unsigned>(o.alpn.size())) == 0; // 0 means success here
        }
    }

    SSL* ssl = ok ? SSL_new(ctx) : nullptr;
    SSL_CTX_free(ctx); // the SSL object keeps its own reference
    if (ssl == nullptr) {
        err = tlsErrorText();
        return false;
    }

    if (!server && *host != '\0') {
        unsigned char scratch[sizeof(in6_addr)];
        const bool isIp = inet_pton(AF_INET, host, scratch) == 1 || inet_pton(AF_INET6, host, scratch) == 1;
        if (isIp) { // no SNI for IP literals: match the certificate's IP entries instead
            if (o.verify) {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host);
            }
        } else {
            SSL_set_tlsext_host_name(ssl, host);
            if (o.verify) {
                SSL_set1_host(ssl, host);
            }
        }
    }

    SSL_set_fd(ssl, static_cast<int>(s.fd));
    s.ssl = ssl;
    if (!tlsHandshake(s, server, err)) {
        SSL_free(s.ssl);
        s.ssl = nullptr;
        ERR_clear_error();
        return false; // the connection is unusable now: close the socket
    }
    return true;
}
#endif

long long ioRecv(Socket& s, char* buf, std::size_t n) {
#ifdef SONATA_NET_TLS
    if (s.ssl != nullptr) {
        return tlsResult(s, SSL_read(s.ssl, buf, static_cast<int>(std::min<std::size_t>(n, 1 << 30))), true);
    }
#endif
    return sockRecv(s.fd, buf, n);
}

long long ioSend(Socket& s, const char* buf, std::size_t n) {
#ifdef SONATA_NET_TLS
    if (s.ssl != nullptr) {
        return tlsResult(s, SSL_write(s.ssl, buf, static_cast<int>(std::min<std::size_t>(n, 1 << 30))), true);
    }
#endif
    return sockSend(s.fd, buf, n);
}

// True when recv() can return without touching the network (also used by net.select).
bool hasBuffered(const Socket& s) {
    if (!s.pending.empty()) {
        return true;
    }
#ifdef SONATA_NET_TLS
    return s.ssl != nullptr && SSL_pending(s.ssl) > 0;
#else
    return false;
#endif
}

// Reads one chunk into s.pending. Returns bytes read, 0 on EOF, -1 on error.
long long fill(Socket& s, std::string& err) {
    char buf[16384];
    const long long n = runIO(s, false, err, [&] { return ioRecv(s, buf, sizeof buf); });
    if (n > 0) {
        s.pending.append(buf, static_cast<std::size_t>(n));
    }
    return n;
}

// Up to "want" bytes; with "exact", waits for all of them (fewer only on EOF). Empty out = EOF.
bool readBytes(Socket& s, std::size_t want, bool exact, std::string& out, std::string& err) {
    while (want > 0 && (s.pending.empty() || (exact && s.pending.size() < want))) {
        const long long n = fill(s, err);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            break;
        }
    }

    const std::size_t take = std::min(want, s.pending.size());
    out.assign(s.pending, 0, take);
    s.pending.erase(0, take);
    return true;
}

// Everything up to and including "delim". Data stays buffered if this fails.
bool readUntil(Socket& s, std::string_view delim, std::size_t limit, std::string& out, std::string& err) {
    std::size_t from = 0;

    for (;;) {
        const std::size_t at = s.pending.find(delim, from);
        if (at != std::string::npos) {
            if (at + delim.size() > limit) {
                err = "limit exceeded";
                return false;
            }
            out.assign(s.pending, 0, at + delim.size());
            s.pending.erase(0, at + delim.size());
            return true;
        }

        if (s.pending.size() >= limit) {
            err = "limit exceeded";
            return false;
        }

        from = s.pending.size() >= delim.size() ? s.pending.size() - delim.size() + 1 : 0;

        const long long n = fill(s, err);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            err = kClosed;
            return false;
        }
    }
}

bool writeAll(Socket& s, const char* data, std::size_t len, std::size_t& sent, std::string& err) {
    sent = 0;
    while (sent < len) {
        const long long n = runIO(s, true, err, [&] { return ioSend(s, data + sent, len - sent); });
        if (n < 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool sendDatagram(Socket& s, const char* host, int port, const char* data, std::size_t len, std::size_t& sent, std::string& err) {
    int flags = 0;
#ifdef AI_V4MAPPED
    if (s.family == AF_INET6) {
        flags = AI_V4MAPPED;
    }
#endif
    const AddrList list = lookup(host, port, s.family, SOCK_DGRAM, flags, err);
    if (!list) {
        return false;
    }

    const addrinfo* a = list.get();
    const long long n = runIO(s, true, err, [&] {
        return static_cast<long long>(::sendto(s.fd, data, len, 0, a->ai_addr, static_cast<SockLen>(a->ai_addrlen)));
    });
    if (n < 0) {
        return false;
    }
    sent = static_cast<std::size_t>(n);
    return true;
}

bool recvDatagram(Socket& s, std::size_t max, std::string& data, std::string& host, int& port, std::string& err) {
    data.resize(max);
    sockaddr_storage from{};
    SockLen fromLen = 0;

    const long long n = runIO(s, false, err, [&] {
        fromLen = sizeof from;
        return static_cast<long long>(::recvfrom(s.fd, &data[0], max, 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
    });
    if (n < 0) {
        return false;
    }

    data.resize(static_cast<std::size_t>(n));
    if (!addressText(reinterpret_cast<const sockaddr*>(&from), fromLen, host, port)) {
        host.clear();
        port = 0;
    }
    return true;
}

// Lua bindings: net /////////////////////////////////////

int failure(lua_State* L, std::string_view message) {
    lua_pushnil(L);
    lua_pushlstring(L, message.data(), message.size());
    return 2;
}

// Creates a Socket userdata (pushed on the stack) whose destructor closes the descriptor.
Socket* newSocket(lua_State* L, Kind kind) {
    void* memory = lua_newuserdatadtor(L, sizeof(Socket), [](lua_State*, void* p) { static_cast<Socket*>(p)->~Socket(); });
    Socket* s = new (memory) Socket();
    s->kind = kind;

    lua_getfield(L, LUA_REGISTRYINDEX, kSocketMeta);
    lua_setmetatable(L, -2);
    return s;
}

Socket* toSocket(lua_State* L, int idx) {
    void* p = lua_touserdata(L, idx);
    if (p == nullptr || !lua_getmetatable(L, idx)) {
        return nullptr;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kSocketMeta);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same ? static_cast<Socket*>(p) : nullptr;
}

Socket* checkSocket(lua_State* L, int idx, std::optional<Kind> need = std::nullopt, bool open = true) {
    auto* s = static_cast<Socket*>(luaL_checkudata(L, idx, kSocketMeta));
    luaL_argcheck(L, !need || s->kind == *need, idx, need == Kind::Udp ? "expected a UDP socket" : "expected a TCP socket");
    luaL_argcheck(L, !open || s->fd != kInvalid, idx, "socket is closed");
    return s;
}

Socket* checkDatagram(lua_State* L, int idx) {
    Socket* s = checkSocket(L, idx);
    luaL_argcheck(L, s->kind != Kind::Tcp, idx, "expected a UDP or raw socket");
    return s;
}

double optTimeout(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) {
        return -1;
    }
    const double seconds = luaL_checknumber(L, idx);
    luaL_argcheck(L, seconds >= 0, idx, "timeout must not be negative");
    return seconds;
}

// net.connect(host: string, port: number, timeout: number?): Socket | (nil, string)
int netConnect(lua_State* L) {
    const char* host = luaL_checkstring(L, 1);
    const int port = luaL_checkinteger(L, 2);
    const double timeout = optTimeout(L, 3);

    luaL_argcheck(L, port >= 1 && port <= 65535, 2, "port must be between 1 and 65535");

    Socket* s = newSocket(L, Kind::Tcp);
    s->timeout = timeout;

    std::string err;
    if (!openConnect(*s, host, port, err)) {
        lua_pop(L, 1);
        return failure(L, err);
    }
    return 1;
}

// net.listen(host: string?, port: number, backlog: number?): Socket | (nil, string)
int netListen(lua_State* L) {
    const char* host = luaL_optstring(L, 1, nullptr);
    const int port = luaL_checkinteger(L, 2);
    const int backlog = luaL_optinteger(L, 3, 128);

    luaL_argcheck(L, port >= 0 && port <= 65535, 2, "port must be between 0 and 65535");
    luaL_argcheck(L, backlog >= 1 && backlog <= 65535, 3, "backlog must be between 1 and 65535");

    Socket* s = newSocket(L, Kind::Tcp);

    std::string err;
    if (!openBound(*s, host, port, backlog, kReuseListen, err)) {
        lua_pop(L, 1);
        return failure(L, err);
    }
    return 1;
}

// net.udp(host: string?, port: number?, reuse: boolean?): Socket | (nil, string)
int netUdp(lua_State* L) {
    const char* host = luaL_optstring(L, 1, nullptr);
    const int port = luaL_optinteger(L, 2, 0);
    const bool reuse = lua_toboolean(L, 3) != 0;

    luaL_argcheck(L, port >= 0 && port <= 65535, 2, "port must be between 0 and 65535");

    Socket* s = newSocket(L, Kind::Udp);

    std::string err;
    if (!openUdp(*s, host, port, reuse, err)) {
        lua_pop(L, 1);
        return failure(L, err);
    }
    return 1;
}

// net.raw(protocol: string | number, family: string?): Socket | (nil, string)
int netRaw(lua_State* L) {
    static const char* const kNames[] = {"icmp", "icmpv6", "tcp", "udp", nullptr};
    static const int kProtocols[] = {IPPROTO_ICMP, IPPROTO_ICMPV6, IPPROTO_TCP, IPPROTO_UDP};
    static const char* const kFamilies[] = {"ipv4", "ipv6", nullptr};

    int protocol = 0;
    const char* defaultFamily = "ipv4";
    if (lua_type(L, 1) == LUA_TNUMBER) {
        protocol = luaL_checkinteger(L, 1);
        luaL_argcheck(L, protocol >= 0 && protocol <= 255, 1, "protocol number must be between 0 and 255");
    } else {
        const int which = luaL_checkoption(L, 1, nullptr, kNames);
        protocol = kProtocols[which];
        defaultFamily = which == 1 ? "ipv6" : "ipv4";
    }
    const int family = luaL_checkoption(L, 2, defaultFamily, kFamilies) == 1 ? AF_INET6 : AF_INET;

    Socket* s = newSocket(L, Kind::Raw);

    std::string err;
    if (!openRaw(*s, family, protocol, err)) {
        lua_pop(L, 1);
        return failure(L, err);
    }
    return 1;
}

// net.resolve(host: string, family: string?): { string } | (nil, string)
int netResolve(lua_State* L) {
    static const char* const kFamilies[] = {"any", "ipv4", "ipv6", nullptr};
    static const int kValues[] = {AF_UNSPEC, AF_INET, AF_INET6};

    const char* host = luaL_checkstring(L, 1);
    const int family = kValues[luaL_checkoption(L, 2, "any", kFamilies)];

    std::string err;
    const AddrList list = lookup(host, 0, family, SOCK_STREAM, 0, err);
    if (!list) {
        return failure(L, err);
    }

    lua_createtable(L, 0, 0);

    int count = 0;
    for (const addrinfo* a = list.get(); a != nullptr; a = a->ai_next) {
        std::string text;
        int port = 0;
        if (addressText(a->ai_addr, static_cast<SockLen>(a->ai_addrlen), text, port)) {
            lua_pushlstring(L, text.data(), text.size());
            lua_rawseti(L, -2, ++count);
        }
    }
    return 1;
}

// net.select(readers: { Socket }, writers: { Socket }?, timeout: number?): ({ Socket }, { Socket }) | (nil, string)
// A socket is "readable" when recv/accept would not wait (data buffered or arriving, or a closed peer).
int netSelect(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
    }
    const double timeout = optTimeout(L, 3);
    const bool hasWriters = lua_istable(L, 2);
    lua_settop(L, 3);

    const int counts[2] = {static_cast<int>(lua_objlen(L, 1)), hasWriters ? static_cast<int>(lua_objlen(L, 2)) : 0};

    for (int t = 0; t < 2; ++t) {
        for (int i = 1; i <= counts[t]; ++i) {
            lua_rawgeti(L, t + 1, i);
            if (toSocket(L, -1) == nullptr) {
                luaL_error(L, "net.select: entry %d of argument #%d is not a socket", i, t + 1);
            }
            lua_pop(L, 1);
        }
    }

    std::vector<PollFd> fds;
    std::vector<const Socket*> socks;
    bool buffered = false;

    for (int t = 0; t < 2; ++t) {
        for (int i = 1; i <= counts[t]; ++i) {
            lua_rawgeti(L, t + 1, i);
            const Socket* s = toSocket(L, -1);
            lua_pop(L, 1);

            PollFd p{};
            p.fd = s->fd;
            p.events = t == 0 ? POLLIN : POLLOUT;
            fds.push_back(p);
            socks.push_back(s);
            buffered = buffered || (t == 0 && hasBuffered(*s));
        }
    }

    int rc = 0;
    while (!fds.empty()) {
        rc = pollSockets(fds.data(), fds.size(), buffered ? 0 : toMillis(timeout));
        if (rc >= 0 || !interrupted(lastError())) {
            break;
        }
    }
    if (rc < 0) {
        return failure(L, errorText(lastError()));
    }

    lua_createtable(L, 0, 0); // 4: readable
    lua_createtable(L, 0, 0); // 5: writable
    int outCount[2] = {0, 0};

    for (std::size_t k = 0; k < fds.size(); ++k) {
        const int t = k < static_cast<std::size_t>(counts[0]) ? 0 : 1;
        const Socket* s = socks[k];
        const short ev = fds[k].revents;
        const bool ready = s->fd == kInvalid || (t == 0 && hasBuffered(*s)) || (ev & (t == 0 ? POLLIN : POLLOUT)) ||
                           (ev & (POLLERR | POLLHUP | POLLNVAL));
        if (!ready) {
            continue;
        }

        const int index = t == 0 ? static_cast<int>(k) + 1 : static_cast<int>(k) - counts[0] + 1;
        lua_rawgeti(L, t + 1, index);
        lua_rawseti(L, 4 + t, ++outCount[t]);
    }
    return 2;
}

// Lua bindings: Socket methods /////////////////////////////////////////7

// sock:close()
int sockClose(lua_State* L) {
    checkSocket(L, 1, std::nullopt, false)->close();
    return 0;
}

// sock:setTimeout(seconds: number?)
int sockSetTimeout(lua_State* L) {
    Socket* s = checkSocket(L, 1, std::nullopt, false);
    s->timeout = optTimeout(L, 2);
    return 0;
}

// sock:send(data: string): number | (nil, string, number)
int sockSend(lua_State* L) {
    Socket* s = checkSocket(L, 1, Kind::Tcp);
    std::size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);

    std::string err;
    std::size_t sent = 0;
    if (!writeAll(*s, data, len, sent, err)) {
        failure(L, err);
        lua_pushnumber(L, static_cast<double>(sent));
        return 3;
    }

    lua_pushnumber(L, static_cast<double>(sent));
    return 1;
}

// sock:recv(max: number?, exact: boolean?): string | (nil, string)
int sockRecvMethod(lua_State* L) {
    Socket* s = checkSocket(L, 1, Kind::Tcp);
    const int max = luaL_optinteger(L, 2, 4096);
    const bool exact = lua_toboolean(L, 3) != 0;

    luaL_argcheck(L, max >= 0 && static_cast<std::size_t>(max) <= kMaxBuffer, 2, "max out of range");

    std::string out;
    std::string err;
    if (!readBytes(*s, static_cast<std::size_t>(max), exact, out, err)) {
        return failure(L, err);
    }

    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// sock:recvUntil(delimiter: string, max: number?): string | (nil, string)
int sockRecvUntil(lua_State* L) {
    Socket* s = checkSocket(L, 1, Kind::Tcp);
    std::size_t delimLength = 0;
    const char* delim = luaL_checklstring(L, 2, &delimLength);
    const int limit = luaL_optinteger(L, 3, 1 << 20);

    luaL_argcheck(L, delimLength != 0, 2, "delimiter must not be empty");
    luaL_argcheck(L, limit >= 1 && static_cast<std::size_t>(limit) <= kMaxBuffer, 3, "max out of range");

    std::string out;
    std::string err;
    if (!readUntil(*s, std::string_view(delim, delimLength), static_cast<std::size_t>(limit), out, err)) {
        return failure(L, err);
    }

    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// server:accept(): Socket | (nil, string)
int sockAccept(lua_State* L) {
    Socket* server = checkSocket(L, 1, Kind::Tcp);
    Socket* client = newSocket(L, Kind::Tcp);

    std::string err;
    if (!acceptOne(*server, *client, err)) {
        lua_pop(L, 1);
        return failure(L, err);
    }
    return 1;
}

// sock:sendTo(data: string, host: string, port: number?): number | (nil, string)
int sockSendTo(lua_State* L) {
    Socket* s = checkDatagram(L, 1);
    std::size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    const char* host = luaL_checkstring(L, 3);
    const int port = luaL_optinteger(L, 4, 0);

    luaL_argcheck(L, len <= 65507, 2, "datagram too large");
    luaL_argcheck(L, port >= (s->kind == Kind::Raw ? 0 : 1) && port <= 65535, 4, "port must be between 1 and 65535 (raw sockets take none)");

    std::string err;
    std::size_t sent = 0;
    if (!sendDatagram(*s, host, port, data, len, sent, err)) {
        return failure(L, err);
    }

    lua_pushnumber(L, static_cast<double>(sent));
    return 1;
}

// sock:recvFrom(max: number?): (string, string, number) | (nil, string)
int sockRecvFrom(lua_State* L) {
    Socket* s = checkDatagram(L, 1);
    const int max = luaL_optinteger(L, 2, 2048);

    luaL_argcheck(L, max >= 1 && max <= 65535, 2, "max must be between 1 and 65535");

    std::string data;
    std::string host;
    std::string err;
    int port = 0;
    if (!recvDatagram(*s, static_cast<std::size_t>(max), data, host, port, err)) {
        return failure(L, err);
    }

    lua_pushlstring(L, data.data(), data.size());
    lua_pushlstring(L, host.data(), host.size());
    lua_pushinteger(L, port);
    return 3;
}

// sock:shutdown(how: string?): true | (nil, string)
int sockShutdown(lua_State* L) {
    static const char* const kHow[] = {"write", "read", "both", nullptr};
#ifdef _WIN32
    static const int kModes[] = {SD_SEND, SD_RECEIVE, SD_BOTH};
#else
    static const int kModes[] = {SHUT_WR, SHUT_RD, SHUT_RDWR};
#endif

    Socket* s = checkSocket(L, 1);
    const int mode = kModes[luaL_checkoption(L, 2, "write", kHow)];

#ifdef SONATA_NET_TLS
    if (s->ssl != nullptr && mode != kModes[1]) { // closing our side: tell the peer with close_notify
        SSL_shutdown(s->ssl);
        ERR_clear_error();
    }
#endif
    if (::shutdown(s->fd, mode) != 0) {
        return failure(L, errorText(lastError()));
    }
    lua_pushboolean(L, 1);
    return 1;
}

// sock:setOption(name: string, value: boolean | number): true | (nil, string)
// Booleans: nodelay, keepalive, broadcast, hdrincl (IPv4 raw), multicastLoop. Number (0-255): multicastTtl.
int sockSetOption(lua_State* L) {
    static const char* const kNames[] = {"nodelay", "keepalive", "broadcast", "hdrincl", "multicastLoop", "multicastTtl", nullptr};
    static const int kLevels[] = {IPPROTO_TCP, SOL_SOCKET, SOL_SOCKET, IPPROTO_IP};
    static const int kOptions[] = {TCP_NODELAY, SO_KEEPALIVE, SO_BROADCAST, IP_HDRINCL};

    Socket* s = checkSocket(L, 1);
    const int which = luaL_checkoption(L, 2, nullptr, kNames);
    int number = 0;
    if (which == 5) {
        number = luaL_checkinteger(L, 3);
        luaL_argcheck(L, number >= 0 && number <= 255, 3, "ttl must be between 0 and 255");
    } else {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        number = lua_toboolean(L, 3) != 0 ? 1 : 0;
    }
    luaL_argcheck(L, which != 3 || (s->kind == Kind::Raw && s->family == AF_INET), 2, "hdrincl needs an IPv4 raw socket");

    std::string err;
    if (which < 4) {
        if (!setFlag(s->fd, kLevels[which], kOptions[which], number != 0)) {
            return failure(L, errorText(lastError()));
        }
    } else if (!setMulticast(*s, which == 4 ? Mcast::Loop : Mcast::Ttl, number, nullptr, err)) {
        return failure(L, err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int addressOf(lua_State* L, bool peer) {
    Socket* s = checkSocket(L, 1);

    sockaddr_storage ss{};
    SockLen len = sizeof ss;
    auto* addr = reinterpret_cast<sockaddr*>(&ss);
    const int rc = peer ? ::getpeername(s->fd, addr, &len) : ::getsockname(s->fd, addr, &len);
    if (rc != 0) {
        return failure(L, errorText(lastError()));
    }

    std::string host;
    int port = 0;
    if (!addressText(addr, len, host, port)) {
        return failure(L, "unknown address family");
    }

    lua_pushlstring(L, host.data(), host.size());
    lua_pushinteger(L, port);
    return 2;
}

// sock:localAddress(): (string, number) | (nil, string)
int sockLocalAddress(lua_State* L) { return addressOf(L, false); }

// sock:peerAddress(): (string, number) | (nil, string)
int sockPeerAddress(lua_State* L) { return addressOf(L, true); }

int changeGroupMethod(lua_State* L, bool join) {
    Socket* s = checkSocket(L, 1, Kind::Udp);
    const char* group = luaL_checkstring(L, 2);
    const char* iface = luaL_optstring(L, 3, nullptr);

    std::string err;
    if (!changeGroup(*s, group, iface, join, err)) {
        return failure(L, err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

// sock:joinGroup(group: string, interface: string?): true | (nil, string)
int sockJoinGroup(lua_State* L) { return changeGroupMethod(L, true); }

// sock:leaveGroup(group: string, interface: string?): true | (nil, string)
int sockLeaveGroup(lua_State* L) { return changeGroupMethod(L, false); }

// sock:setMulticastInterface(interface: string): true | (nil, string)
int sockSetMulticastInterface(lua_State* L) {
    Socket* s = checkSocket(L, 1, Kind::Udp);
    const char* iface = luaL_checkstring(L, 2);

    std::string err;
    if (!setMulticast(*s, Mcast::Interface, 0, iface, err)) {
        return failure(L, err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

#ifdef SONATA_NET_TLS
// Optional string field of the table at index "t"; the value stays on the stack, which keeps it alive.
const char* tlsStringField(lua_State* L, int t, const char* name) {
    lua_getfield(L, t, name);
    if (lua_isnil(L, -1)) {
        return nullptr;
    }
    if (!lua_isstring(L, -1)) {
        luaL_error(L, "startTls: '%s' must be a string", name);
    }
    return lua_tolstring(L, -1, nullptr);
}
#endif

// sock:startTls(options: { host: string?, verify: boolean?, caFile: string?, cert: string?, key: string?, alpn: { string }? }?):
//     true | (nil, string)
int sockStartTls(lua_State* L) {
    Socket* s = checkSocket(L, 1, Kind::Tcp);
#ifdef SONATA_NET_TLS
    TlsOptions o;
    int alpnIndex = 0;

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        o.host = tlsStringField(L, 2, "host");
        o.caFile = tlsStringField(L, 2, "caFile");
        o.cert = tlsStringField(L, 2, "cert");
        o.key = tlsStringField(L, 2, "key");

        lua_getfield(L, 2, "verify");
        o.verify = lua_isnil(L, -1) || lua_toboolean(L, -1) != 0;

        lua_getfield(L, 2, "alpn");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                luaL_error(L, "startTls: 'alpn' must be a list of strings");
            }
            alpnIndex = lua_gettop(L);
            const int n = static_cast<int>(lua_objlen(L, alpnIndex));
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, alpnIndex, i);
                std::size_t len = 0;
                if (!lua_isstring(L, -1) || (lua_tolstring(L, -1, &len), len == 0 || len > 255)) {
                    luaL_error(L, "startTls: 'alpn' entries must be strings of 1 to 255 bytes");
                }
                lua_pop(L, 1);
            }
        }
    }

    std::string alpn;
    if (alpnIndex != 0) {
        const int n = static_cast<int>(lua_objlen(L, alpnIndex));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, alpnIndex, i);
            std::size_t len = 0;
            const char* name = lua_tolstring(L, -1, &len);
            alpn += static_cast<char>(len);
            alpn.append(name, len);
            lua_pop(L, 1);
        }
        o.alpn = alpn;
    }

    std::string err;
    if (!startTls(*s, o, err)) {
        return failure(L, err);
    }
    lua_pushboolean(L, 1);
    return 1;
#else
    (void)s;
    return failure(L, "TLS is not available: build net.cpp with SONATA_NET_TLS and link OpenSSL");
#endif
}

// sock:tlsInfo(): { version: string, cipher: string, alpn: string? } | nil
int sockTlsInfo(lua_State* L) {
    Socket* s = checkSocket(L, 1, Kind::Tcp);
#ifdef SONATA_NET_TLS
    if (s->ssl != nullptr) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, SSL_get_version(s->ssl));
        lua_setfield(L, -2, "version");
        lua_pushstring(L, SSL_get_cipher_name(s->ssl));
        lua_setfield(L, -2, "cipher");

        const unsigned char* proto = nullptr;
        unsigned protoLength = 0;
        SSL_get0_alpn_selected(s->ssl, &proto, &protoLength);
        if (proto != nullptr) {
            lua_pushlstring(L, reinterpret_cast<const char*>(proto), protoLength);
            lua_setfield(L, -2, "alpn");
        }
        return 1;
    }
#endif
    (void)s;
    lua_pushnil(L);
    return 1;
}

// URL helpers ///////////////////////

using OptView = std::optional<std::string_view>;

struct UrlParts {
    OptView scheme, username, password, host, query, fragment;
    std::string_view path;
    int port = -1;
};

constexpr bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }
constexpr char toLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

int hexValue(char c) {
    if (isDigit(c)) {
        return c - '0';
    }
    const char lower = toLower(c);
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

// Everything except A-Z a-z 0-9 - . _ ~ and the characters in "safe" becomes %XX.
void percentEncode(std::string& out, std::string_view text, std::string_view safe) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const char c : text) {
        const bool keep = isAlpha(c) || isDigit(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
                          safe.find(c) != std::string_view::npos;
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

void addPair(std::string& out, std::string_view key, std::string_view value) {
    if (!out.empty()) {
        out += '&';
    }
    percentEncode(out, key, {});
    out += '=';
    percentEncode(out, value, {});
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

// Appends the pairs of the table at absolute index "t": first the array part ({key, value} entries),
// then the string keys. Returns an error message or nullptr.
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

    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            std::size_t keyLength = 0;
            std::size_t valueLength = 0;
            const char* key = lua_tolstring(L, -2, &keyLength);
            const char* value = scalarText(L, -1, &valueLength);
            if (value == nullptr) {
                return "query values must be strings, numbers or booleans";
            }
            addPair(out, std::string_view(key, keyLength), std::string_view(value, valueLength));
        }
        lua_pop(L, 1);
    }
    return nullptr;
}

void setField(lua_State* L, const char* name, const OptView& value, bool lower = false) {
    if (!value) {
        return;
    }
    if (lower) {
        std::string text(*value);
        std::transform(text.begin(), text.end(), text.begin(), toLower);
        lua_pushlstring(L, text.data(), text.size());
    } else {
        lua_pushlstring(L, value->data(), value->size());
    }
    lua_setfield(L, -2, name);
}

// Optional string field of the table at index 1. The value is left on the stack on purpose (it keeps the string alive).
OptView stringField(lua_State* L, const char* name) {
    lua_getfield(L, 1, name);
    if (lua_isnil(L, -1)) {
        return std::nullopt;
    }
    if (!lua_isstring(L, -1)) {
        luaL_error(L, "url.build: '%s' must be a string", name);
    }
    std::size_t len = 0;
    const char* text = lua_tolstring(L, -1, &len);
    return std::string_view(text, len);
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
            lua_pushlstring(L, out.data(), out.size());
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
    lua_pushlstring(L, out.data(), out.size());
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
    lua_pushlstring(L, out.data(), out.size());
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

        lua_pushlstring(L, key.data(), key.size());
        lua_pushvalue(L, -1);
        lua_rawget(L, 2);
        const bool seen = !lua_isnil(L, -1);
        lua_pop(L, 1); // stack: key

        lua_pushlstring(L, value.data(), value.size()); // key, value

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

// net.url.buildQuery(pairs: { { string } } | { [string]: string | number | boolean }): string
int urlBuildQuery(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    const char* err = nullptr;
    {
        std::string out;
        err = appendQuery(L, 1, out);
        if (err == nullptr) {
            lua_pushlstring(L, out.data(), out.size());
            return 1;
        }
    }
    luaL_error(L, "url.buildQuery: %s", err);
    return 0;
}

constexpr NativeFunction kFunctions[] = {
    {"connect", netConnect},
    {"listen", netListen},
    {"udp", netUdp},
    {"resolve", netResolve},
    {"select", netSelect},
    {"raw", netRaw},
};

constexpr NativeFunction kSocketMethods[] = {
    {"close", sockClose},
    {"setTimeout", sockSetTimeout},
    {"send", sockSend},
    {"recv", sockRecvMethod},
    {"recvUntil", sockRecvUntil},
    {"accept", sockAccept},
    {"sendTo", sockSendTo},
    {"recvFrom", sockRecvFrom},
    {"shutdown", sockShutdown},
    {"setOption", sockSetOption},
    {"localAddress", sockLocalAddress},
    {"peerAddress", sockPeerAddress},
    {"startTls", sockStartTls},
    {"tlsInfo", sockTlsInfo},
    {"joinGroup", sockJoinGroup},
    {"leaveGroup", sockLeaveGroup},
    {"setMulticastInterface", sockSetMulticastInterface},
};

constexpr NativeFunction kUrlFunctions[] = {
    {"parse", urlParse},
    {"build", urlBuild},
    {"encode", urlEncode},
    {"decode", urlDecode},
    {"parseQuery", urlParseQuery},
    {"buildQuery", urlBuildQuery},
};

} // namespace

void openNet(lua_State* L) {
    ensureNetworking();

    // Socket metatable (kept in the registry, so each VM has its own).
    luaL_newmetatable(L, kSocketMeta);
    lua_createtable(L, 0, static_cast<int>(std::size(kSocketMethods)));
    setFunctions(L, kSocketMethods);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 2);
    setFunctions(L, kFunctions);

    lua_createtable(L, 0, static_cast<int>(std::size(kUrlFunctions)));
    setFunctions(L, kUrlFunctions);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "url");
}

} // namespace sonata::lib::libs
