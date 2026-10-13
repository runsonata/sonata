// @sonata/net sockets: TCP, UDP, raw, Unix domain sockets and TLS.
//
// Every socket is non-blocking. Operations run on the VM's thread and, when the
// OS says they would block, wait through the scheduler (sonata/core/scheduler.hpp):
// a task waiting for a socket lets the other tasks run. Work that cannot be done
// without blocking (DNS lookups, connecting through proxies, TLS handshakes) runs
// on a worker thread while the socket is marked busy.

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
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
#if __has_include(<afunix.h>)
#include <afunix.h>
#define SONATA_HAVE_UNIX_SOCKETS 1
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define SONATA_HAVE_UNIX_SOCKETS 1
#endif

#ifdef SONATA_NET_TLS
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#include <sonata/core/scheduler.hpp>
#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs::net {

namespace {

using sched::Scheduler;

constexpr double kForever = std::numeric_limits<double>::infinity();

// Platform layer ////////////////////////////////////////////////////////////

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

void setError(int code) { WSASetLastError(code); }
constexpr int kErrWouldBlock = WSAEWOULDBLOCK;
constexpr int kErrFailed = WSAECONNRESET;
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

void prepare(SockT fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

void setError(int code) { errno = code; }
constexpr int kErrWouldBlock = EAGAIN;
constexpr int kErrFailed = EPROTO;
#endif

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

int toMillis(double seconds) {
    if (seconds < 0 || seconds >= kForever) {
        return -1;
    }
    return static_cast<int>(std::min(std::ceil(seconds * 1000.0), 2147483000.0));
}

// Sockets //////////////////////////////////////////////////////////////////

constexpr const char* kSocketMeta = "sonata.net.Socket";
constexpr const char* kTlsContextMeta = "sonata.net.TlsContext";
constexpr const char* kTimedOut = "timed out";
constexpr const char* kClosed = "closed";
constexpr std::size_t kMaxBuffer = 64u * 1024 * 1024;
#ifdef _WIN32
constexpr bool kReuseListen = false; // on Windows SO_REUSEADDR would let other processes steal the port
#else
constexpr bool kReuseListen = true;  // a restarted server can rebind while old connections linger
#endif

enum class Kind { Tcp, Udp, Raw, Unix };

const char* kindName(Kind kind) {
    switch (kind) {
    case Kind::Tcp: return "tcp";
    case Kind::Udp: return "udp";
    case Kind::Raw: return "raw";
    case Kind::Unix: return "unix";
    }
    return "?";
}

bool isStream(Kind kind) {
    return kind == Kind::Tcp || kind == Kind::Unix;
}

#ifdef SONATA_NET_TLS
// A TLS configuration, shareable between connections (net.tls.context).
struct TlsContext {
    SSL_CTX* ctx = nullptr;
    bool server = false;
    bool verify = true;
    std::string alpn; // servers: the protocols we pick from (the ALPN callback reads it)
    TlsContext() = default;
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    ~TlsContext() {
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }
};
using TlsContextPtr = std::shared_ptr<TlsContext>;
#endif

// The socket itself. Shared between the Lua object and the worker jobs using it.
struct Socket {
    SockT fd = kInvalid;
    Kind kind = Kind::Tcp;
    int family = AF_INET;
    double timeout = -1;    // seconds; < 0 waits forever
    std::string pending;    // received from the OS but not yet consumed (streams)
    std::string host;       // name given to connect(): default for TLS server-name checks
    std::string tlsError;   // set by the TLS layer when an operation fails for good
    std::string unixPath;   // a Unix listener's socket file, removed on close
    bool accepted = false;  // came from accept(): startTls() then acts as the TLS server
    bool wantWrite = false; // which readiness to wait for (the TLS layer may flip it)
    bool busy = false;      // a worker job (TLS handshake) is using it
    bool closeLater = false; // close() was called while busy
#ifdef SONATA_NET_TLS
    SSL* ssl = nullptr;
#endif

    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    bool open() const { return fd != kInvalid; }

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
#if SONATA_HAVE_UNIX_SOCKETS && !defined(_WIN32)
        if (!unixPath.empty()) {
            ::unlink(unixPath.c_str());
            unixPath.clear();
        }
#endif
    }
};

using SocketPtr = std::shared_ptr<Socket>;

struct SocketBox {
    SocketPtr socket;
};

// Blocking waits, for worker threads ///////////////////////////////////////

enum class Wait { Ready, Timeout, Failed };

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
// (up to "timeout" each time) whenever the OS says it would block. Blocks: only
// for worker threads.
template <class Op>
long long blockingIO(Socket& s, bool forWrite, double timeout, std::string& err, Op op) {
    for (;;) {
        s.wantWrite = forWrite;
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

        const Wait w = waitFor(s.fd, s.wantWrite, timeout);
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
#if SONATA_HAVE_UNIX_SOCKETS
    if (addr->sa_family == AF_UNIX) {
        const auto* un = reinterpret_cast<const sockaddr_un*>(addr);
        const std::size_t offset = offsetof(sockaddr_un, sun_path);
        host = len > static_cast<SockLen>(offset) ? std::string(un->sun_path, strnlen(un->sun_path, static_cast<std::size_t>(len) - offset)) : std::string();
        port = 0;
        return true;
    }
#endif
    char h[NI_MAXHOST];
    char p[NI_MAXSERV];
    if (getnameinfo(addr, len, h, sizeof h, p, sizeof p, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }
    host = h;
    port = std::atoi(p);
    return true;
}

// Finishes a non-blocking connect(), waiting up to "timeout".
bool connectWait(Socket& s, const sockaddr* addr, SockLen len, double timeout, std::string& err) {
    if (::connect(s.fd, addr, len) == 0) {
        return true;
    }

    const int code = lastError();
    if (!inProgress(code)) {
        err = errorText(code);
        return false;
    }

    const Wait w = waitFor(s.fd, true, timeout);
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

struct ConnectTarget {
    std::string host;
    int port = 0;
    int family = AF_UNSPEC;
    std::string localAddress;
    int localPort = 0;
};

// Tries every address the name resolves to, in order. Blocking (worker thread).
bool openConnect(Socket& s, const ConnectTarget& target, double timeout, std::string& err) {
    const AddrList list = lookup(target.host.c_str(), target.port, target.family, SOCK_STREAM, 0, err);
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

        if (!target.localAddress.empty() || target.localPort != 0) {
            std::string bindErr;
            const AddrList local = lookup(target.localAddress.empty() ? nullptr : target.localAddress.c_str(), target.localPort,
                                          a->ai_family, SOCK_STREAM, AI_PASSIVE | AI_NUMERICHOST, bindErr);
            if (!local || ::bind(s.fd, local->ai_addr, static_cast<SockLen>(local->ai_addrlen)) != 0) {
                err = local ? errorText(lastError()) : bindErr;
                s.close();
                continue;
            }
        }

        if (connectWait(s, a->ai_addr, static_cast<SockLen>(a->ai_addrlen), timeout, err)) {
            return true;
        }
        s.close();
    }
    return false;
}

#if SONATA_HAVE_UNIX_SOCKETS
bool unixAddress(const std::string& path, sockaddr_un& addr, SockLen& len, std::string& err) {
    addr = sockaddr_un{};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        err = "socket path must be 1 to " + std::to_string(sizeof(addr.sun_path) - 1) + " bytes";
        return false;
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    len = static_cast<SockLen>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

bool openUnixConnect(Socket& s, const std::string& path, double timeout, std::string& err) {
    sockaddr_un addr{};
    SockLen len = 0;
    if (!unixAddress(path, addr, len, err)) {
        return false;
    }
    s.fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s.fd == kInvalid) {
        err = errorText(lastError());
        return false;
    }
    s.family = AF_UNIX;
    s.kind = Kind::Unix;
    prepare(s.fd);
    if (!connectWait(s, reinterpret_cast<const sockaddr*>(&addr), len, timeout, err)) {
        s.close();
        return false;
    }
    return true;
}
#endif

// Binds s (and starts listening when backlog > 0).
bool bindOne(Socket& s, const char* host, int port, int family, int backlog, bool reuse, bool reusePort, bool v6only,
             std::string& err) {
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
        }
#ifdef SO_REUSEPORT
        if (reusePort || (reuse && s.kind == Kind::Udp)) {
            setFlag(s.fd, SOL_SOCKET, SO_REUSEPORT, true); // BSD/macOS need this to share a multicast port
        }
#else
        (void)reusePort;
#endif
        if (a->ai_family == AF_INET6) {
            setFlag(s.fd, IPPROTO_IPV6, IPV6_V6ONLY, v6only); // "::" also accepts IPv4 unless v6only
        }

        if (::bind(s.fd, a->ai_addr, static_cast<SockLen>(a->ai_addrlen)) == 0 && (backlog == 0 || ::listen(s.fd, backlog) == 0)) {
            return true;
        }
        err = errorText(lastError());
        s.close();
    }
    return false;
}

struct BindOptions {
    int backlog = 0;
    bool reuse = false;
    bool reusePort = false;
    bool v6only = false;
    int family = AF_UNSPEC;
};

// No host: wildcard address, IPv6 (dual-stack) first, IPv4 as the fallback.
bool openBound(Socket& s, const char* host, int port, const BindOptions& o, std::string& err) {
    if (host != nullptr || o.family != AF_UNSPEC) {
        return bindOne(s, host, port, o.family, o.backlog, o.reuse, o.reusePort, o.v6only, err);
    }
    return bindOne(s, nullptr, port, AF_INET6, o.backlog, o.reuse, o.reusePort, o.v6only, err) ||
           bindOne(s, nullptr, port, AF_INET, o.backlog, o.reuse, o.reusePort, o.v6only, err);
}

bool openUdp(Socket& s, const char* host, int port, const BindOptions& o, std::string& err) {
    if (host == nullptr && port == 0) { // unbound
        const int family = o.family == AF_INET6 ? AF_INET6 : AF_INET;
        s.fd = ::socket(family, SOCK_DGRAM, 0);
        if (s.fd == kInvalid) {
            err = errorText(lastError());
            return false;
        }
        s.family = family;
        prepare(s.fd);
        return true;
    }
    return openBound(s, host, port, o, err);
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

// TLS ////////////////////////////////////////////////////////////////////////

struct TlsOptions {
    std::optional<std::string> host;
    std::string caFile, caPem, certFile, keyFile, certPem, keyPem;
    std::string alpn; // wire format: length-prefixed protocol names
    bool verify = true;
    bool verifyClient = false;
    bool server = false;
    int minVersion = 0x0303; // TLS 1.2
#ifdef SONATA_NET_TLS
    TlsContextPtr context;   // a ready context (net.tls.context) instead of the fields above
#endif
};

#ifdef SONATA_NET_TLS
// The TLS layer reports through the errno convention: it sets the error to "would block" (and picks the
// direction to wait for) or to "failed" (with the message in Socket::tlsError).
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

// Converts an OpenSSL result: >= 0 is success (0 = clean EOF when "io"), -1 comes with the error set.
long long tlsResult(Socket& s, int r, bool io) {
    if (r > 0) {
        return r;
    }

    const int sys = lastError();
    switch (SSL_get_error(s.ssl, r)) {
    case SSL_ERROR_WANT_READ:
        s.wantWrite = false;
        setError(kErrWouldBlock);
        return -1;
    case SSL_ERROR_WANT_WRITE:
        s.wantWrite = true;
        setError(kErrWouldBlock);
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
    setError(kErrFailed);
    return -1;
}

// Loads every certificate of a PEM text. The first is returned; the rest go to "chain".
bool readPemCertificates(const std::string& pem, std::vector<X509*>& out) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return false;
    }
    while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        out.push_back(cert);
    }
    BIO_free(bio);
    ERR_clear_error(); // the read that ended the loop
    return !out.empty();
}

TlsContextPtr makeContext(const TlsOptions& o, std::string& err) {
    auto context = std::make_shared<TlsContext>();
    context->server = o.server;
    context->verify = o.verify;
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (ctx == nullptr) {
        err = tlsErrorText();
        return nullptr;
    }
    context->ctx = ctx;
    SSL_CTX_set_min_proto_version(ctx, o.minVersion);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF); // many servers just close the TCP connection
#endif

    bool ok = true;

    // Our own certificate (servers; clients for mutual TLS).
    if (!o.certPem.empty()) {
        std::vector<X509*> certs;
        ok = readPemCertificates(o.certPem, certs) && SSL_CTX_use_certificate(ctx, certs[0]) == 1;
        for (std::size_t i = 1; ok && i < certs.size(); ++i) {
            ok = SSL_CTX_add_extra_chain_cert(ctx, certs[i]) == 1; // takes ownership
            certs[i] = nullptr;
        }
        for (X509* cert : certs) {
            X509_free(cert);
        }
        if (!ok && err.empty()) {
            err = "invalid certPem: " + tlsErrorText();
        }
    } else if (!o.certFile.empty()) {
        ok = SSL_CTX_use_certificate_chain_file(ctx, o.certFile.c_str()) == 1;
        if (!ok) {
            err = "cannot load cert '" + o.certFile + "': " + tlsErrorText();
        }
    }
    if (ok && (!o.keyPem.empty() || !o.keyFile.empty() || !o.certFile.empty() || !o.certPem.empty())) {
        if (!o.keyPem.empty()) {
            BIO* bio = BIO_new_mem_buf(o.keyPem.data(), static_cast<int>(o.keyPem.size()));
            EVP_PKEY* key = bio != nullptr ? PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr) : nullptr;
            ok = key != nullptr && SSL_CTX_use_PrivateKey(ctx, key) == 1;
            EVP_PKEY_free(key);
            BIO_free(bio);
            if (!ok) {
                err = "invalid keyPem: " + tlsErrorText();
            }
        } else if (!o.keyFile.empty() || !o.certFile.empty()) {
            const std::string& keyFile = o.keyFile.empty() ? o.certFile : o.keyFile;
            ok = SSL_CTX_use_PrivateKey_file(ctx, keyFile.c_str(), SSL_FILETYPE_PEM) == 1;
            if (!ok) {
                err = "cannot load key '" + keyFile + "': " + tlsErrorText();
            }
        }
        if (ok && SSL_CTX_check_private_key(ctx) != 1) {
            ok = false;
            err = "the key does not match the certificate";
        }
    }

    // Who we trust.
    const bool verifying = o.server ? o.verifyClient : o.verify;
    if (ok && verifying) {
        if (!o.caPem.empty()) {
            std::vector<X509*> certs;
            ok = readPemCertificates(o.caPem, certs);
            X509_STORE* store = SSL_CTX_get_cert_store(ctx);
            for (X509* cert : certs) {
                ok = ok && X509_STORE_add_cert(store, cert) == 1;
                X509_free(cert);
            }
            if (!ok) {
                err = "invalid caPem: " + tlsErrorText();
            }
        } else if (!o.caFile.empty()) {
            ok = SSL_CTX_load_verify_locations(ctx, o.caFile.c_str(), nullptr) == 1;
            if (!ok) {
                err = "cannot load caFile '" + o.caFile + "': " + tlsErrorText();
            }
        } else {
            ok = SSL_CTX_set_default_verify_paths(ctx) == 1;
        }
        if (ok) {
            SSL_CTX_set_verify(ctx, o.server ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_PEER, nullptr);
        }
    }

    if (ok && !o.alpn.empty()) {
        if (o.server) {
            context->alpn = o.alpn;
            // Pick the first protocol of ours that the client offers.
            SSL_CTX_set_alpn_select_cb(
                ctx,
                [](SSL*, const unsigned char** out, unsigned char* outLength, const unsigned char* in, unsigned int inLength,
                   void* arg) {
                    const auto* ours = static_cast<const std::string*>(arg);
                    unsigned char* selected = nullptr;
                    if (SSL_select_next_proto(&selected, outLength, reinterpret_cast<const unsigned char*>(ours->data()),
                                              static_cast<unsigned>(ours->size()), in, inLength) != OPENSSL_NPN_NEGOTIATED) {
                        return SSL_TLSEXT_ERR_NOACK;
                    }
                    *out = selected;
                    return SSL_TLSEXT_ERR_OK;
                },
                &context->alpn);
        } else {
            ok = SSL_CTX_set_alpn_protos(ctx, reinterpret_cast<const unsigned char*>(o.alpn.data()),
                                         static_cast<unsigned>(o.alpn.size())) == 0; // 0 means success here
        }
    }

    if (!ok) {
        if (err.empty()) {
            err = tlsErrorText();
        }
        return nullptr;
    }
    return context;
}

// Runs the handshake (blocking: worker thread). Client handshake on connected sockets,
// server handshake on accepted ones.
bool handshake(Socket& s, const TlsOptions& o, std::string& err) {
    if (s.ssl != nullptr) {
        err = "TLS already started";
        return false;
    }
    if (!s.pending.empty()) {
        err = "unread data is buffered; read it before upgrading to TLS";
        return false;
    }

    const bool server = s.accepted;
    TlsContextPtr context = o.context;
    if (!context) {
        TlsOptions own = o;
        own.server = server;
        if (server && own.certFile.empty() && own.certPem.empty()) {
            err = "TLS on an accepted socket needs a certificate (cert or certPem)";
            return false;
        }
        context = makeContext(own, err);
        if (!context) {
            return false;
        }
    } else if (context->server != server) {
        err = server ? "this socket came from accept(): it needs a server context (server = true)"
                     : "a server context can only be used on sockets returned by accept()";
        return false;
    }

    const std::string host = o.host ? *o.host : s.host;
    if (!server && context->verify && host.empty()) {
        err = "host is required to verify the certificate (pass host, or verify = false)";
        return false;
    }

    SSL* ssl = SSL_new(context->ctx);
    if (ssl == nullptr) {
        err = tlsErrorText();
        return false;
    }

    if (!server && !host.empty()) {
        unsigned char scratch[sizeof(in6_addr)];
        const bool isIp = inet_pton(AF_INET, host.c_str(), scratch) == 1 || inet_pton(AF_INET6, host.c_str(), scratch) == 1;
        if (isIp) { // no SNI for IP literals: match the certificate's IP entries instead
            if (context->verify) {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str());
            }
        } else {
            SSL_set_tlsext_host_name(ssl, host.c_str());
            if (context->verify) {
                SSL_set1_host(ssl, host.c_str());
            }
        }
    }

    SSL_set_fd(ssl, static_cast<int>(s.fd));
    s.ssl = ssl;
    const bool ok = blockingIO(s, false, s.timeout, err, [&] {
                        return tlsResult(s, server ? SSL_accept(s.ssl) : SSL_connect(s.ssl), false);
                    }) >= 0;
    if (!ok) {
        SSL_free(s.ssl);
        s.ssl = nullptr;
        ERR_clear_error();
        return false;
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

// Proxies (blocking: worker thread) ////////////////////////////////////////

struct Proxy {
    std::string type; // "socks5" | "http"
    std::string host;
    int port = 0;
    std::string username;
    std::string password;
};

bool sendBlocking(Socket& s, std::string_view data, double timeout, std::string& err) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const long long n = blockingIO(s, true, timeout, err, [&] { return ioSend(s, data.data() + sent, data.size() - sent); });
        if (n < 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Reads exactly "count" more bytes into s.pending.
bool fillBlocking(Socket& s, std::size_t count, double timeout, std::string& err) {
    char buf[4096];
    while (s.pending.size() < count) {
        const long long n = blockingIO(s, false, timeout, err, [&] { return ioRecv(s, buf, sizeof buf); });
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            err = "the proxy closed the connection";
            return false;
        }
        s.pending.append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

std::string takeBytes(Socket& s, std::size_t count) {
    std::string out = s.pending.substr(0, count);
    s.pending.erase(0, count);
    return out;
}

const char* socksReply(int code) {
    static const char* const kReplies[] = {"succeeded",
                                           "general SOCKS server failure",
                                           "connection not allowed by ruleset",
                                           "network unreachable",
                                           "host unreachable",
                                           "connection refused",
                                           "TTL expired",
                                           "command not supported",
                                           "address type not supported"};
    return code >= 0 && code < 9 ? kReplies[code] : "unknown SOCKS error";
}

bool socks5Handshake(Socket& s, const Proxy& proxy, const std::string& host, int port, double timeout, std::string& err) {
    const bool auth = !proxy.username.empty();
    std::string hello = auth ? std::string("\x05\x02\x00\x02", 4) : std::string("\x05\x01\x00", 3);
    if (!sendBlocking(s, hello, timeout, err) || !fillBlocking(s, 2, timeout, err)) {
        return false;
    }
    const std::string choice = takeBytes(s, 2);
    if (choice[0] != 5 || static_cast<unsigned char>(choice[1]) == 0xFF) {
        err = "the SOCKS proxy accepted none of our authentication methods";
        return false;
    }
    if (choice[1] == 2) {
        if (proxy.username.size() > 255 || proxy.password.size() > 255) {
            err = "SOCKS username and password are limited to 255 bytes";
            return false;
        }
        std::string login = "\x01";
        login += static_cast<char>(proxy.username.size());
        login += proxy.username;
        login += static_cast<char>(proxy.password.size());
        login += proxy.password;
        if (!sendBlocking(s, login, timeout, err) || !fillBlocking(s, 2, timeout, err)) {
            return false;
        }
        if (takeBytes(s, 2)[1] != 0) {
            err = "the SOCKS proxy rejected the username or password";
            return false;
        }
    }

    std::string request("\x05\x01\x00", 3);
    unsigned char address[16];
    if (inet_pton(AF_INET, host.c_str(), address) == 1) {
        request += '\x01';
        request.append(reinterpret_cast<const char*>(address), 4);
    } else if (inet_pton(AF_INET6, host.c_str(), address) == 1) {
        request += '\x04';
        request.append(reinterpret_cast<const char*>(address), 16);
    } else {
        if (host.size() > 255) {
            err = "host name too long for SOCKS";
            return false;
        }
        request += '\x03';
        request += static_cast<char>(host.size());
        request += host; // the proxy resolves it
    }
    request += static_cast<char>((port >> 8) & 0xFF);
    request += static_cast<char>(port & 0xFF);

    if (!sendBlocking(s, request, timeout, err) || !fillBlocking(s, 5, timeout, err)) {
        return false;
    }
    const int reply = static_cast<unsigned char>(s.pending[1]);
    if (reply != 0) {
        err = std::string("SOCKS proxy: ") + socksReply(reply);
        return false;
    }
    std::size_t length = 0;
    switch (s.pending[3]) {
    case 1: length = 4 + 4 + 2; break;
    case 4: length = 4 + 16 + 2; break;
    case 3: length = 4 + 1 + static_cast<unsigned char>(s.pending[4]) + 2; break;
    default:
        err = "malformed SOCKS reply";
        return false;
    }
    if (!fillBlocking(s, length, timeout, err)) {
        return false;
    }
    takeBytes(s, length);
    return true;
}

bool httpConnectHandshake(Socket& s, const Proxy& proxy, const std::string& host, int port, double timeout, std::string& err) {
    const bool ipv6 = host.find(':') != std::string::npos;
    const std::string authority = (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    std::string request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
    if (!proxy.username.empty()) {
        request += "Proxy-Authorization: Basic " + base64Encode(proxy.username + ":" + proxy.password) + "\r\n";
    }
    request += "\r\n";
    if (!sendBlocking(s, request, timeout, err)) {
        return false;
    }

    char buf[4096];
    for (;;) {
        const std::size_t end = s.pending.find("\r\n\r\n");
        if (end != std::string::npos) {
            const std::string head = takeBytes(s, end + 4);
            const std::size_t space = head.find(' ');
            const int status = space == std::string::npos ? 0 : std::atoi(head.c_str() + space + 1);
            if (status < 200 || status > 299) {
                err = "the HTTP proxy refused the tunnel: " + head.substr(0, head.find("\r\n"));
                return false;
            }
            return true; // anything after the head already belongs to the tunnel
        }
        if (s.pending.size() > 64 * 1024) {
            err = "the HTTP proxy's response is too large";
            return false;
        }
        const long long n = blockingIO(s, false, timeout, err, [&] { return ioRecv(s, buf, sizeof buf); });
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            err = "the proxy closed the connection";
            return false;
        }
        s.pending.append(buf, static_cast<std::size_t>(n));
    }
}

// Lua: sockets //////////////////////////////////////////////////////////////

SocketBox* newSocketBox(lua_State* L, SocketPtr socket) {
    void* memory = lua_newuserdatadtor(L, sizeof(SocketBox), [](lua_State*, void* p) { static_cast<SocketBox*>(p)->~SocketBox(); });
    auto* box = new (memory) SocketBox();
    box->socket = std::move(socket);
    luaL_getmetatable(L, kSocketMeta);
    lua_setmetatable(L, -2);
    return box;
}

Socket* toSocket(lua_State* L, int idx) {
    void* p = lua_touserdata(L, idx);
    if (p == nullptr || !lua_getmetatable(L, idx)) {
        return nullptr;
    }
    luaL_getmetatable(L, kSocketMeta);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same ? static_cast<SocketBox*>(p)->socket.get() : nullptr;
}

Socket& socketAt(lua_State* L, int idx) {
    return *static_cast<SocketBox*>(luaL_checkudata(L, idx, kSocketMeta))->socket;
}

// Checks the socket argument: the right kind, open, and not busy with a handshake.
Socket& checkSocket(lua_State* L, int idx, int kinds = 0xF) {
    Socket& s = socketAt(L, idx);
    if ((kinds & (1 << static_cast<int>(s.kind))) == 0) {
        const bool streams = (kinds & (1 << static_cast<int>(Kind::Tcp))) != 0;
        const bool only = kinds == (1 << static_cast<int>(Kind::Tcp)) || kinds == (1 << static_cast<int>(Kind::Udp));
        luaL_argerror(L, idx, only ? (streams ? "expected a TCP socket" : "expected a UDP socket")
                                   : (streams ? "expected a stream (TCP or Unix) socket" : "expected a UDP or raw socket"));
    }
    luaL_argcheck(L, s.open(), idx, "socket is closed");
    luaL_argcheck(L, !s.busy, idx, "socket is busy (a TLS handshake is in progress)");
    return s;
}

constexpr int kStreams = (1 << static_cast<int>(Kind::Tcp)) | (1 << static_cast<int>(Kind::Unix));
constexpr int kDatagrams = (1 << static_cast<int>(Kind::Udp)) | (1 << static_cast<int>(Kind::Raw));

double optTimeout(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) {
        return -1;
    }
    const double seconds = luaL_checknumber(L, idx);
    luaL_argcheck(L, seconds >= 0, idx, "timeout must not be negative");
    return seconds;
}

// The deadline for the next wait of "s" on the scheduler's clock.
double deadlineFor(lua_State* L, const Socket& s) {
    return s.timeout < 0 ? kForever : Scheduler::of(L).now() + s.timeout;
}

constexpr int kTimedOutCode = -3;

// The operation would block: waits (the continuation "k" retries it). Returns
// kTimedOutCode if the deadline has passed, sched::kInline after an inline wait
// (retry right away), or the yield.
int waitSocket(lua_State* L, Socket& s, int deadlineSlot, sched::Continuation k) {
    Scheduler& scheduler = Scheduler::of(L);
    const double deadline = lua_tonumber(L, deadlineSlot);
    if (scheduler.now() >= deadline) {
        return kTimedOutCode;
    }
    return scheduler.waitIo(L, static_cast<sched::SocketHandle>(s.fd), s.wantWrite ? sched::kWritable : sched::kReadable,
                            deadline, k);
}

void refreshDeadline(lua_State* L, const Socket& s, int deadlineSlot) {
    lua_pushnumber(L, deadlineFor(L, s));
    lua_replace(L, deadlineSlot);
}

// Socket state after a wait: closed (by another task) or busy means stop.
Socket* afterWait(lua_State* L) {
    Socket& s = socketAt(L, 1);
    return s.open() && !s.busy ? &s : nullptr;
}

enum class Io { Done, Blocked, Failed };

// One non-blocking read into s.pending. "got": bytes read (0 = EOF).
Io readSome(Socket& s, long long& got, std::string& err) {
    char buf[16384];
    s.wantWrite = false;
    got = ioRecv(s, buf, sizeof buf);
    if (got >= 0) {
        if (got > 0) {
            s.pending.append(buf, static_cast<std::size_t>(got));
        }
        return Io::Done;
    }
    const int code = lastError();
    if (wouldBlock(code)) {
        return Io::Blocked;
    }
    err = s.tlsError.empty() ? errorText(code) : s.tlsError;
    s.tlsError.clear();
    return Io::Failed;
}

// sock:send ------------------------------------------------------------------
// Stack: 1 socket, 2 data, 3 deadline, 4 bytes sent so far

int sendStep(lua_State* L);
int sendK(lua_State* L, int) {
    lua_settop(L, 4);
    return sendStep(L);
}

int sendFailed(lua_State* L, std::string_view message) {
    const double sent = lua_tonumber(L, 4);
    failure(L, message);
    lua_pushnumber(L, sent);
    return 3;
}

int sendStep(lua_State* L) {
    std::size_t length = 0;
    const char* data = lua_tolstring(L, 2, &length);
    for (;;) {
        Socket* s = afterWait(L);
        if (s == nullptr) {
            return sendFailed(L, kClosed);
        }
        auto sent = static_cast<std::size_t>(lua_tonumber(L, 4));
        if (sent >= length) {
            lua_pushnumber(L, static_cast<double>(sent));
            return 1;
        }

        s->wantWrite = true;
        const long long n = ioSend(*s, data + sent, length - sent);
        if (n >= 0) {
            lua_pushnumber(L, static_cast<double>(sent + static_cast<std::size_t>(n)));
            lua_replace(L, 4);
            refreshDeadline(L, *s, 3);
            continue;
        }
        const int code = lastError();
        if (!wouldBlock(code)) {
            const std::string err = s->tlsError.empty() ? errorText(code) : s->tlsError;
            s->tlsError.clear();
            return sendFailed(L, err);
        }
        const int r = waitSocket(L, *s, 3, sendK);
        if (r == kTimedOutCode) {
            return sendFailed(L, kTimedOut);
        }
        if (r != sched::kInline) {
            return r;
        }
    }
}

// sock:send(data: string): number | (nil, string, number)
int sockSend(lua_State* L) {
    Socket& s = checkSocket(L, 1, kStreams);
    luaL_checkstring(L, 2);
    lua_settop(L, 2);
    lua_pushnumber(L, deadlineFor(L, s)); // 3
    lua_pushnumber(L, 0);                 // 4
    return sendStep(L);
}

// sock:recv ------------------------------------------------------------------
// Stack: 1 socket, 2 max, 3 exact, 4 deadline

int recvStep(lua_State* L);
int recvK(lua_State* L, int) {
    lua_settop(L, 4);
    return recvStep(L);
}

int recvStep(lua_State* L) {
    const auto want = static_cast<std::size_t>(lua_tonumber(L, 2));
    const bool exact = lua_toboolean(L, 3) != 0;
    for (;;) {
        Socket* s = afterWait(L);
        if (s == nullptr) {
            return failure(L, kClosed);
        }
        if (want == 0 || (!s->pending.empty() && (!exact || s->pending.size() >= want))) {
            const std::size_t take = std::min(want, s->pending.size());
            pushString(L, std::string_view(s->pending).substr(0, take));
            s->pending.erase(0, take);
            return 1;
        }

        long long got = 0;
        std::string err;
        const Io io = readSome(*s, got, err);
        if (io == Io::Failed) {
            return failure(L, err);
        }
        if (io == Io::Done) {
            if (got == 0) { // EOF: whatever is left ("" means the peer closed the connection)
                pushString(L, s->pending);
                s->pending.clear();
                return 1;
            }
            refreshDeadline(L, *s, 4);
            continue;
        }
        const int r = waitSocket(L, *s, 4, recvK);
        if (r == kTimedOutCode) {
            return failure(L, kTimedOut);
        }
        if (r != sched::kInline) {
            return r;
        }
    }
}

// sock:recv(max: number?, exact: boolean?): string | (nil, string)
int sockRecv(lua_State* L) {
    Socket& s = checkSocket(L, 1, kStreams);
    const int max = luaL_optinteger(L, 2, 4096);
    luaL_argcheck(L, max >= 0 && static_cast<std::size_t>(max) <= kMaxBuffer, 2, "max out of range");
    const bool exact = lua_toboolean(L, 3) != 0;
    lua_settop(L, 1);
    lua_pushnumber(L, max);
    lua_pushboolean(L, exact);
    lua_pushnumber(L, deadlineFor(L, s));
    return recvStep(L);
}

// sock:recvUntil / sock:recvLine ------------------------------------------------
// Stack: 1 socket, 2 delimiter, 3 limit, 4 deadline, 5 search from, 6 strip (recvLine)

int untilStep(lua_State* L);
int untilK(lua_State* L, int) {
    lua_settop(L, 6);
    return untilStep(L);
}

int untilStep(lua_State* L) {
    std::size_t delimLength = 0;
    const char* delimText = lua_tolstring(L, 2, &delimLength);
    const std::string_view delim(delimText, delimLength);
    const auto limit = static_cast<std::size_t>(lua_tonumber(L, 3));
    const bool strip = lua_toboolean(L, 6) != 0;

    for (;;) {
        Socket* s = afterWait(L);
        if (s == nullptr) {
            return failure(L, kClosed);
        }
        const auto from = static_cast<std::size_t>(lua_tonumber(L, 5));
        const std::size_t at = s->pending.find(delim, from);
        if (at != std::string::npos) {
            if (at + delim.size() > limit) {
                return failure(L, "limit exceeded");
            }
            std::size_t keep = at + delim.size();
            if (strip) {
                keep = at > 0 && s->pending[at - 1] == '\r' ? at - 1 : at;
            }
            pushString(L, std::string_view(s->pending).substr(0, keep));
            s->pending.erase(0, at + delim.size());
            return 1;
        }
        if (s->pending.size() >= limit) {
            return failure(L, "limit exceeded");
        }
        lua_pushnumber(L, static_cast<double>(s->pending.size() >= delim.size() ? s->pending.size() - delim.size() + 1 : 0));
        lua_replace(L, 5);

        long long got = 0;
        std::string err;
        const Io io = readSome(*s, got, err);
        if (io == Io::Failed) {
            return failure(L, err);
        }
        if (io == Io::Done) {
            if (got == 0) {
                if (strip && !s->pending.empty()) { // a last line without an ending
                    pushString(L, s->pending);
                    s->pending.clear();
                    return 1;
                }
                return failure(L, kClosed);
            }
            refreshDeadline(L, *s, 4);
            continue;
        }
        const int r = waitSocket(L, *s, 4, untilK);
        if (r == kTimedOutCode) {
            return failure(L, kTimedOut);
        }
        if (r != sched::kInline) {
            return r;
        }
    }
}

int startUntil(lua_State* L, const char* delim, std::size_t delimLength, int limitArg, bool strip) {
    Socket& s = checkSocket(L, 1, kStreams);
    const int limit = luaL_optinteger(L, limitArg, 1 << 20);
    luaL_argcheck(L, limit >= 1 && static_cast<std::size_t>(limit) <= kMaxBuffer, limitArg, "max out of range");

    std::string delimiter(delim, delimLength); // the argument is about to be dropped from the stack
    lua_settop(L, 1);
    pushString(L, delimiter);
    lua_pushnumber(L, limit);
    lua_pushnumber(L, deadlineFor(L, s));
    lua_pushnumber(L, 0);
    lua_pushboolean(L, strip);
    return untilStep(L);
}

// sock:recvUntil(delimiter: string, max: number?): string | (nil, string)
int sockRecvUntil(lua_State* L) {
    std::size_t delimLength = 0;
    const char* delim = luaL_checklstring(L, 2, &delimLength);
    luaL_argcheck(L, delimLength != 0, 2, "delimiter must not be empty");
    return startUntil(L, delim, delimLength, 3, false);
}

// sock:recvLine(max: number?): string | (nil, string)
int sockRecvLine(lua_State* L) {
    return startUntil(L, "\n", 1, 2, true);
}

// sock:recvAll --------------------------------------------------------------
// Stack: 1 socket, 2 limit, 3 deadline

int recvAllStep(lua_State* L);
int recvAllK(lua_State* L, int) {
    lua_settop(L, 3);
    return recvAllStep(L);
}

int recvAllStep(lua_State* L) {
    const auto limit = static_cast<std::size_t>(lua_tonumber(L, 2));
    for (;;) {
        Socket* s = afterWait(L);
        if (s == nullptr) {
            return failure(L, kClosed);
        }
        if (s->pending.size() > limit) {
            return failure(L, "limit exceeded");
        }
        long long got = 0;
        std::string err;
        const Io io = readSome(*s, got, err);
        if (io == Io::Failed) {
            return failure(L, err);
        }
        if (io == Io::Done) {
            if (got == 0) {
                pushString(L, s->pending);
                s->pending.clear();
                return 1;
            }
            refreshDeadline(L, *s, 3);
            continue;
        }
        const int r = waitSocket(L, *s, 3, recvAllK);
        if (r == kTimedOutCode) {
            return failure(L, kTimedOut);
        }
        if (r != sched::kInline) {
            return r;
        }
    }
}

// sock:recvAll(max: number?): string | (nil, string)
int sockRecvAll(lua_State* L) {
    Socket& s = checkSocket(L, 1, kStreams);
    const double limit = luaL_optnumber(L, 2, static_cast<double>(kMaxBuffer));
    luaL_argcheck(L, limit >= 0 && limit <= static_cast<double>(kMaxBuffer), 2, "max out of range");
    lua_settop(L, 1);
    lua_pushnumber(L, limit);
    lua_pushnumber(L, deadlineFor(L, s));
    return recvAllStep(L);
}

// server:accept ----------------------------------------------------------------
// Stack: 1 server, 2 deadline

int acceptStep(lua_State* L);
int acceptK(lua_State* L, int) {
    lua_settop(L, 2);
    return acceptStep(L);
}

int acceptStep(lua_State* L) {
    for (;;) {
        Socket* server = afterWait(L);
        if (server == nullptr) {
            return failure(L, kClosed);
        }
        server->wantWrite = false;
        const SockT fd = ::accept(server->fd, nullptr, nullptr);
        if (fd != kInvalid) {
            prepare(fd);
            auto client = std::make_shared<Socket>();
            client->fd = fd;
            client->kind = server->kind;
            client->family = server->family;
            client->timeout = server->timeout;
            client->accepted = true;
            newSocketBox(L, std::move(client));
            return 1;
        }
        const int code = lastError();
#ifndef _WIN32
        if (code == ECONNABORTED || code == EPROTO) {
            continue; // the client gave up before we accepted: wait for the next one
        }
#endif
        if (!wouldBlock(code)) {
            return failure(L, errorText(code));
        }
        const int r = waitSocket(L, *server, 2, acceptK);
        if (r == kTimedOutCode) {
            return failure(L, kTimedOut);
        }
        if (r != sched::kInline) {
            return r;
        }
    }
}

// server:accept(timeout: number?): Socket | (nil, string)
int sockAccept(lua_State* L) {
    Socket& s = checkSocket(L, 1, kStreams);
    const double timeout = lua_isnoneornil(L, 2) ? s.timeout : optTimeout(L, 2);
    lua_settop(L, 1);
    lua_pushnumber(L, timeout < 0 ? kForever : Scheduler::of(L).now() + timeout);
    return acceptStep(L);
}

// sock:sendTo -------------------------------------------------------------------
// Stack: 1 socket, 2 data, 3 host, 4 port, 5 deadline, 6 address (sockaddr bytes, once known)

int sendToStep(lua_State* L);

// After the lookup job: its results are above slot 6 (address | nil, error).
int takeLookup(lua_State* L) {
    if (lua_type(L, 7) != LUA_TSTRING) {
        const std::string err = lua_isstring(L, 8) ? lua_tostring(L, 8) : "lookup failed";
        lua_settop(L, 6);
        return failure(L, err);
    }
    lua_pushvalue(L, 7);
    lua_replace(L, 6);
    lua_settop(L, 6);
    return 0;
}

int sendToK(lua_State* L, int) {
    if (lua_gettop(L) > 6) {
        if (const int n = takeLookup(L)) {
            return n;
        }
    }
    lua_settop(L, 6);
    return sendToStep(L);
}

int sendToStep(lua_State* L) {
    for (;;) {
        Socket* s = afterWait(L);
        if (s == nullptr) {
            return failure(L, kClosed);
        }

        if (lua_isnil(L, 6)) {
            // Resolve the destination on a worker (getaddrinfo blocks).
            const std::string host = lua_tostring(L, 3);
            const int port = static_cast<int>(lua_tonumber(L, 4));
            int flags = 0;
#ifdef AI_V4MAPPED
            if (s->family == AF_INET6) {
                flags = AI_V4MAPPED;
            }
#endif
            const int family = s->family;
            const int r = Scheduler::of(L).await(L, [host, port, family, flags]() -> sched::Completion {
                std::string err;
                const AddrList list = lookup(host.c_str(), port, family, SOCK_DGRAM, flags, err);
                std::string address;
                if (list) {
                    address.assign(reinterpret_cast<const char*>(list->ai_addr), list->ai_addrlen);
                }
                return [address, err](lua_State* T) {
                    if (address.empty()) {
                        lua_pushnil(T);
                        pushString(T, err);
                        return 2;
                    }
                    pushString(T, address);
                    return 1;
                };
            }, sendToK);
            if (r != sched::kInline) {
                return r;
            }
            if (const int n = takeLookup(L)) {
                return n;
            }
            continue;
        }

        std::size_t length = 0;
        const char* data = lua_tolstring(L, 2, &length);
        std::size_t addressLength = 0;
        const char* address = lua_tolstring(L, 6, &addressLength);

        s->wantWrite = true;
        const long long n = ::sendto(s->fd, data, static_cast<int>(length), 0, reinterpret_cast<const sockaddr*>(address),
                                     static_cast<SockLen>(addressLength));
        if (n >= 0) {
            lua_pushnumber(L, static_cast<double>(n));
            return 1;
        }
        const int code = lastError();
        if (!wouldBlock(code)) {
            return failure(L, errorText(code));
        }
        const int r = waitSocket(L, *s, 5, sendToK);
        if (r == kTimedOutCode) {
            return failure(L, kTimedOut);
        }
        if (r != sched::kInline) {
            return r;
        }
    }
}

// sock:sendTo(data: string, host: string, port: number?): number | (nil, string)
int sockSendTo(lua_State* L) {
    Socket& s = checkSocket(L, 1, kDatagrams);
    std::size_t len = 0;
    luaL_checklstring(L, 2, &len);
    luaL_checkstring(L, 3);
    const int port = luaL_optinteger(L, 4, 0);

    luaL_argcheck(L, len <= 65507, 2, "datagram too large");
    luaL_argcheck(L, port >= (s.kind == Kind::Raw ? 0 : 1) && port <= 65535, 4, "port must be between 1 and 65535 (raw sockets take none)");

    lua_settop(L, 3);
    lua_pushnumber(L, port);
    lua_pushnumber(L, deadlineFor(L, s));
    lua_pushnil(L);
    return sendToStep(L);
}

// sock:recvFrom -------------------------------------------------------------------
// Stack: 1 socket, 2 max, 3 deadline

int recvFromStep(lua_State* L);
int recvFromK(lua_State* L, int) {
    lua_settop(L, 3);
    return recvFromStep(L);
}

int recvFromStep(lua_State* L) {
    const auto max = static_cast<std::size_t>(lua_tonumber(L, 2));
    std::vector<char> data(max);
    for (;;) {
        Socket* s = afterWait(L);
        if (s == nullptr) {
            return failure(L, kClosed);
        }
        sockaddr_storage from{};
        SockLen fromLen = sizeof from;
        s->wantWrite = false;
        const long long n = ::recvfrom(s->fd, data.data(), static_cast<int>(max), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n >= 0) {
            std::string host;
            int port = 0;
            if (!addressText(reinterpret_cast<const sockaddr*>(&from), fromLen, host, port)) {
                host.clear();
                port = 0;
            }
            pushString(L, std::string_view(data.data(), static_cast<std::size_t>(n)));
            pushString(L, host);
            lua_pushinteger(L, port);
            return 3;
        }
        const int code = lastError();
        if (!wouldBlock(code)) {
            return failure(L, errorText(code));
        }
        const int r = waitSocket(L, *s, 3, recvFromK);
        if (r == kTimedOutCode) {
            return failure(L, kTimedOut);
        }
        if (r != sched::kInline) {
            return r;
        }
    }
}

// sock:recvFrom(max: number?): (string, string, number) | (nil, string)
int sockRecvFrom(lua_State* L) {
    Socket& s = checkSocket(L, 1, kDatagrams);
    const int max = luaL_optinteger(L, 2, 2048);
    luaL_argcheck(L, max >= 1 && max <= 65535, 2, "max must be between 1 and 65535");
    lua_settop(L, 1);
    lua_pushnumber(L, max);
    lua_pushnumber(L, deadlineFor(L, s));
    return recvFromStep(L);
}

// TLS options from Lua ----------------------------------------------------------

#ifdef SONATA_NET_TLS
TlsContextPtr* toTlsContext(lua_State* L, int idx) {
    void* p = lua_touserdata(L, idx);
    if (p == nullptr || !lua_getmetatable(L, idx)) {
        return nullptr;
    }
    luaL_getmetatable(L, kTlsContextMeta);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same ? static_cast<TlsContextPtr*>(p) : nullptr;
}
#endif

// Reads a TLS options table (or true / a context) at "idx". Raises on bad values.
TlsOptions readTlsOptions(lua_State* L, int idx, const char* where) {
    TlsOptions o;
    if (lua_isnoneornil(L, idx) || lua_type(L, idx) == LUA_TBOOLEAN) {
        return o;
    }
#ifdef SONATA_NET_TLS
    if (TlsContextPtr* context = toTlsContext(L, idx)) {
        o.context = *context;
        return o;
    }
#endif
    luaL_checktype(L, idx, LUA_TTABLE);
    const int top = lua_gettop(L);
    const auto field = [&](const char* name) -> std::string {
        const auto value = optStringField(L, idx, name, where);
        return value ? std::string(*value) : std::string();
    };
    if (const auto host = optStringField(L, idx, "host", where)) {
        o.host = std::string(*host);
    }
    o.caFile = field("caFile");
    o.caPem = field("caPem");
    o.certFile = field("cert");
    o.keyFile = field("key");
    o.certPem = field("certPem");
    o.keyPem = field("keyPem");

    lua_getfield(L, idx, "verify");
    o.verify = lua_isnil(L, -1) || lua_toboolean(L, -1) != 0;
    lua_getfield(L, idx, "verifyClient");
    o.verifyClient = lua_toboolean(L, -1) != 0;
    lua_getfield(L, idx, "server");
    o.server = lua_toboolean(L, -1) != 0;

    lua_getfield(L, idx, "minVersion");
    if (!lua_isnil(L, -1)) {
        const char* version = lua_tostring(L, -1);
        if (version != nullptr && std::strcmp(version, "1.3") == 0) {
            o.minVersion = 0x0304;
        } else if (version == nullptr || std::strcmp(version, "1.2") != 0) {
            luaL_error(L, "%s: 'minVersion' must be \"1.2\" or \"1.3\"", where);
        }
    }

#ifdef SONATA_NET_TLS
    lua_getfield(L, idx, "context");
    if (!lua_isnil(L, -1)) {
        TlsContextPtr* context = toTlsContext(L, -1);
        if (context == nullptr) {
            luaL_error(L, "%s: 'context' must be a context from net.tls.context()", where);
        }
        o.context = *context;
    }
#endif

    lua_getfield(L, idx, "alpn");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1)) {
            luaL_error(L, "%s: 'alpn' must be a list of strings", where);
        }
        const int list = lua_gettop(L);
        const int n = static_cast<int>(lua_objlen(L, list));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, list, i);
            std::size_t len = 0;
            const char* name = lua_isstring(L, -1) ? lua_tolstring(L, -1, &len) : nullptr;
            if (name == nullptr || len == 0 || len > 255) {
                luaL_error(L, "%s: 'alpn' entries must be strings of 1 to 255 bytes", where);
            }
            o.alpn += static_cast<char>(len);
            o.alpn.append(name, len);
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
    return o;
}

// sock:startTls(options: TlsOptions?): true | (nil, string)
int sockStartTls(lua_State* L) {
    Socket& s = checkSocket(L, 1, 1 << static_cast<int>(Kind::Tcp));
#ifdef SONATA_NET_TLS
    TlsOptions options = readTlsOptions(L, 2, "startTls");
    if (s.ssl != nullptr) {
        return failure(L, "TLS already started");
    }
    if (!s.pending.empty()) {
        return failure(L, "unread data is buffered; read it before upgrading to TLS");
    }

    SocketPtr socket = static_cast<SocketBox*>(lua_touserdata(L, 1))->socket;
    socket->busy = true;
    auto shared = std::make_shared<TlsOptions>(std::move(options));
    return Scheduler::of(L).await(L, [socket, shared]() -> sched::Completion {
        std::string err;
        const bool ok = handshake(*socket, *shared, err);
        return [socket, ok, err](lua_State* T) {
            socket->busy = false;
            if (socket->closeLater) {
                socket->close();
                return failure(T, kClosed);
            }
            if (!ok) {
                return failure(T, err);
            }
            lua_pushboolean(T, 1);
            return 1;
        };
    });
#else
    (void)s;
    return failure(L, "TLS is not available: build net with SONATA_NET_TLS and link OpenSSL");
#endif
}

#ifdef SONATA_NET_TLS
double asn1Seconds(const ASN1_TIME* time) {
    std::tm tm{};
    if (time == nullptr || ASN1_TIME_to_tm(time, &tm) != 1) {
        return 0;
    }
#ifdef _WIN32
    return static_cast<double>(_mkgmtime(&tm));
#else
    return static_cast<double>(timegm(&tm));
#endif
}

std::string nameText(const X509_NAME* name) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        return {};
    }
    X509_NAME_print_ex(bio, name, 0, XN_FLAG_RFC2253);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::string text(data, static_cast<std::size_t>(std::max(length, 0L)));
    BIO_free(bio);
    return text;
}
#endif

// sock:tlsInfo(): { version: string, cipher: string, alpn: string? } | nil
int sockTlsInfo(lua_State* L) {
    Socket& s = checkSocket(L, 1, kStreams);
#ifdef SONATA_NET_TLS
    if (s.ssl != nullptr) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, SSL_get_version(s.ssl));
        lua_setfield(L, -2, "version");
        lua_pushstring(L, SSL_get_cipher_name(s.ssl));
        lua_setfield(L, -2, "cipher");

        const unsigned char* proto = nullptr;
        unsigned protoLength = 0;
        SSL_get0_alpn_selected(s.ssl, &proto, &protoLength);
        if (proto != nullptr) {
            lua_pushlstring(L, reinterpret_cast<const char*>(proto), protoLength);
            lua_setfield(L, -2, "alpn");
        }
        if (const char* server = SSL_get_servername(s.ssl, TLSEXT_NAMETYPE_host_name)) {
            lua_pushstring(L, server);
            lua_setfield(L, -2, "serverName");
        }
        return 1;
    }
#endif
    (void)s;
    lua_pushnil(L);
    return 1;
}

// sock:peerCertificate(): { subject, issuer, serial, notBefore, notAfter, fingerprint, altNames } | nil
int sockPeerCertificate(lua_State* L) {
    Socket& s = checkSocket(L, 1, kStreams);
#ifdef SONATA_NET_TLS
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509* cert = s.ssl != nullptr ? SSL_get1_peer_certificate(s.ssl) : nullptr;
#else
    X509* cert = s.ssl != nullptr ? SSL_get_peer_certificate(s.ssl) : nullptr;
#endif
    if (cert != nullptr) {
        lua_createtable(L, 0, 8);
        setStringField(L, "subject", nameText(X509_get_subject_name(cert)));
        setStringField(L, "issuer", nameText(X509_get_issuer_name(cert)));
        setNumberField(L, "notBefore", asn1Seconds(X509_get0_notBefore(cert)));
        setNumberField(L, "notAfter", asn1Seconds(X509_get0_notAfter(cert)));

        if (BIGNUM* serial = ASN1_INTEGER_to_BN(X509_get0_serialNumber(cert), nullptr)) {
            if (char* hex = BN_bn2hex(serial)) {
                setStringField(L, "serial", hex);
                OPENSSL_free(hex);
            }
            BN_free(serial);
        }

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned digestLength = 0;
        if (X509_digest(cert, EVP_sha256(), digest, &digestLength) == 1) {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string hex;
            for (unsigned i = 0; i < digestLength; ++i) {
                hex += kHex[digest[i] >> 4];
                hex += kHex[digest[i] & 15];
            }
            setStringField(L, "fingerprint", hex);
        }

        lua_createtable(L, 0, 0);
        int count = 0;
        if (auto* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr))) {
            for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
                const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
                if (name->type == GEN_DNS) {
                    const auto* text = reinterpret_cast<const char*>(ASN1_STRING_get0_data(name->d.dNSName));
                    lua_pushlstring(L, text, static_cast<std::size_t>(ASN1_STRING_length(name->d.dNSName)));
                    lua_rawseti(L, -2, ++count);
                } else if (name->type == GEN_IPADD) {
                    const ASN1_OCTET_STRING* ip = name->d.iPAddress;
                    char text[INET6_ADDRSTRLEN] = {};
                    const int family = ASN1_STRING_length(ip) == 4 ? AF_INET : AF_INET6;
                    if (inet_ntop(family, ASN1_STRING_get0_data(ip), text, sizeof text) != nullptr) {
                        lua_pushstring(L, text);
                        lua_rawseti(L, -2, ++count);
                    }
                }
            }
            GENERAL_NAMES_free(names);
        }
        lua_setfield(L, -2, "altNames");
        X509_free(cert);
        return 1;
    }
#endif
    (void)s;
    lua_pushnil(L);
    return 1;
}

// Other socket methods -------------------------------------------------------------

// sock:close()
int sockClose(lua_State* L) {
    Socket& s = socketAt(L, 1);
    if (!s.open()) {
        return 0;
    }
    if (s.busy) {
        // A worker is in the middle of a handshake: make it fail, and close once it is done.
        s.closeLater = true;
#ifdef _WIN32
        ::shutdown(s.fd, SD_BOTH);
#else
        ::shutdown(s.fd, SHUT_RDWR);
#endif
        return 0;
    }
    Scheduler::of(L).wakeIo(static_cast<sched::SocketHandle>(s.fd)); // tasks waiting on it see it closed
    s.close();
    return 0;
}

// sock:isClosed(): boolean
int sockIsClosed(lua_State* L) {
    lua_pushboolean(L, !socketAt(L, 1).open());
    return 1;
}

// sock:kind(): "tcp" | "udp" | "raw" | "unix"
int sockKind(lua_State* L) {
    lua_pushstring(L, kindName(socketAt(L, 1).kind));
    return 1;
}

// sock:setTimeout(seconds: number?)
int sockSetTimeout(lua_State* L) {
    socketAt(L, 1).timeout = optTimeout(L, 2);
    return 0;
}

// sock:getTimeout(): number?
int sockGetTimeout(lua_State* L) {
    const double timeout = socketAt(L, 1).timeout;
    if (timeout < 0) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, timeout);
    }
    return 1;
}

// sock:unread(data: string)  puts data back in front of what recv() returns next
int sockUnread(lua_State* L) {
    Socket& s = checkSocket(L, 1, kStreams);
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 2, &length);
    s.pending.insert(0, data, length);
    return 0;
}

// sock:buffered(): number  bytes received but not read yet
int sockBuffered(lua_State* L) {
    Socket& s = socketAt(L, 1);
    double count = static_cast<double>(s.pending.size());
#ifdef SONATA_NET_TLS
    if (s.ssl != nullptr) {
        count += SSL_pending(s.ssl);
    }
#endif
    lua_pushnumber(L, count);
    return 1;
}

// sock:shutdown(how: string?): true | (nil, string)
int sockShutdown(lua_State* L) {
    static const char* const kHow[] = {"write", "read", "both", nullptr};
#ifdef _WIN32
    static const int kModes[] = {SD_SEND, SD_RECEIVE, SD_BOTH};
#else
    static const int kModes[] = {SHUT_WR, SHUT_RD, SHUT_RDWR};
#endif

    Socket& s = checkSocket(L, 1);
    const int mode = kModes[luaL_checkoption(L, 2, "write", kHow)];

#ifdef SONATA_NET_TLS
    if (s.ssl != nullptr && mode != kModes[1]) { // closing our side: tell the peer with close_notify
        SSL_shutdown(s.ssl);
        ERR_clear_error();
    }
#endif
    if (::shutdown(s.fd, mode) != 0) {
        return failure(L, errorText(lastError()));
    }
    lua_pushboolean(L, 1);
    return 1;
}

enum class OptionType { Flag, Number, Seconds };

struct OptionSpec {
    const char* name;
    int level;
    int option;
    OptionType type;
};

const OptionSpec kOptions[] = {
    {"nodelay", IPPROTO_TCP, TCP_NODELAY, OptionType::Flag},
    {"keepalive", SOL_SOCKET, SO_KEEPALIVE, OptionType::Flag},
    {"broadcast", SOL_SOCKET, SO_BROADCAST, OptionType::Flag},
    {"reuseAddress", SOL_SOCKET, SO_REUSEADDR, OptionType::Flag},
    {"recvBufferSize", SOL_SOCKET, SO_RCVBUF, OptionType::Number},
    {"sendBufferSize", SOL_SOCKET, SO_SNDBUF, OptionType::Number},
    {"linger", SOL_SOCKET, SO_LINGER, OptionType::Seconds},
    {"ttl", IPPROTO_IP, IP_TTL, OptionType::Number},
    {"hdrincl", IPPROTO_IP, IP_HDRINCL, OptionType::Flag},
};

// sock:setOption(name: string, value: boolean | number): true | (nil, string)
int sockSetOption(lua_State* L) {
    Socket& s = checkSocket(L, 1);
    const char* name = luaL_checkstring(L, 2);

    std::string err;
    if (std::strcmp(name, "multicastLoop") == 0 || std::strcmp(name, "multicastTtl") == 0) {
        const bool loop = name[9] == 'L';
        int value = 0;
        if (loop) {
            luaL_checktype(L, 3, LUA_TBOOLEAN);
            value = lua_toboolean(L, 3);
        } else {
            value = luaL_checkinteger(L, 3);
            luaL_argcheck(L, value >= 0 && value <= 255, 3, "ttl must be between 0 and 255");
        }
        if (!setMulticast(s, loop ? Mcast::Loop : Mcast::Ttl, value, nullptr, err)) {
            return failure(L, err);
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    for (const OptionSpec& spec : kOptions) {
        if (std::strcmp(name, spec.name) != 0) {
            continue;
        }
        luaL_argcheck(L, spec.option != IP_HDRINCL || (s.kind == Kind::Raw && s.family == AF_INET), 2, "hdrincl needs an IPv4 raw socket");
        int rc = 0;
        if (spec.type == OptionType::Flag) {
            luaL_checktype(L, 3, LUA_TBOOLEAN);
            rc = setFlag(s.fd, spec.level, spec.option, lua_toboolean(L, 3) != 0) ? 0 : -1;
        } else if (spec.type == OptionType::Number) {
            const int value = luaL_checkinteger(L, 3);
            luaL_argcheck(L, value >= 0, 3, "value must not be negative");
            int level = spec.level;
            int option = spec.option;
            if (spec.option == IP_TTL && s.family == AF_INET6) {
                level = IPPROTO_IPV6;
                option = IPV6_UNICAST_HOPS;
            }
            rc = ::setsockopt(s.fd, level, option, reinterpret_cast<const char*>(&value), sizeof value);
        } else {
            linger value{};
            if (lua_type(L, 3) == LUA_TBOOLEAN && !lua_toboolean(L, 3)) {
                value.l_onoff = 0;
            } else {
                const int seconds = luaL_checkinteger(L, 3);
                luaL_argcheck(L, seconds >= 0, 3, "linger must be false or a number of seconds");
                value.l_onoff = 1;
                value.l_linger = static_cast<decltype(value.l_linger)>(seconds);
            }
            rc = ::setsockopt(s.fd, spec.level, spec.option, reinterpret_cast<const char*>(&value), sizeof value);
        }
        if (rc != 0) {
            return failure(L, errorText(lastError()));
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    luaL_argerror(L, 2, lua_pushfstring(L, "unknown option '%s'", name));
}

// sock:getOption(name: string): boolean | number | (nil, string)
int sockGetOption(lua_State* L) {
    Socket& s = checkSocket(L, 1);
    const char* name = luaL_checkstring(L, 2);
    for (const OptionSpec& spec : kOptions) {
        if (std::strcmp(name, spec.name) != 0) {
            continue;
        }
        if (spec.type == OptionType::Seconds) {
            linger value{};
            SockLen length = sizeof value;
            if (::getsockopt(s.fd, spec.level, spec.option, reinterpret_cast<char*>(&value), &length) != 0) {
                return failure(L, errorText(lastError()));
            }
            if (value.l_onoff == 0) {
                lua_pushboolean(L, 0);
            } else {
                lua_pushnumber(L, value.l_linger);
            }
            return 1;
        }
        int level = spec.level;
        int option = spec.option;
        if (spec.option == IP_TTL && s.family == AF_INET6) {
            level = IPPROTO_IPV6;
            option = IPV6_UNICAST_HOPS;
        }
        int value = 0;
        SockLen length = sizeof value;
        if (::getsockopt(s.fd, level, option, reinterpret_cast<char*>(&value), &length) != 0) {
            return failure(L, errorText(lastError()));
        }
        if (spec.type == OptionType::Flag) {
            lua_pushboolean(L, value != 0);
        } else {
            lua_pushnumber(L, value);
        }
        return 1;
    }
    luaL_argerror(L, 2, lua_pushfstring(L, "unknown option '%s'", name));
}

int addressOf(lua_State* L, bool peer) {
    Socket& s = checkSocket(L, 1);

    sockaddr_storage ss{};
    SockLen len = sizeof ss;
    auto* addr = reinterpret_cast<sockaddr*>(&ss);
    const int rc = peer ? ::getpeername(s.fd, addr, &len) : ::getsockname(s.fd, addr, &len);
    if (rc != 0) {
        return failure(L, errorText(lastError()));
    }

    std::string host;
    int port = 0;
    if (!addressText(addr, len, host, port)) {
        return failure(L, "unknown address family");
    }

    pushString(L, host);
    lua_pushinteger(L, port);
    return 2;
}

// sock:localAddress(): (string, number) | (nil, string)
int sockLocalAddress(lua_State* L) { return addressOf(L, false); }

// sock:peerAddress(): (string, number) | (nil, string)
int sockPeerAddress(lua_State* L) { return addressOf(L, true); }

int changeGroupMethod(lua_State* L, bool join) {
    Socket& s = checkSocket(L, 1, 1 << static_cast<int>(Kind::Udp));
    const char* group = luaL_checkstring(L, 2);
    const char* iface = luaL_optstring(L, 3, nullptr);

    std::string err;
    if (!changeGroup(s, group, iface, join, err)) {
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
    Socket& s = checkSocket(L, 1, 1 << static_cast<int>(Kind::Udp));
    const char* iface = luaL_checkstring(L, 2);

    std::string err;
    if (!setMulticast(s, Mcast::Interface, 0, iface, err)) {
        return failure(L, err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

// tostring(sock): "net.Socket(tcp 127.0.0.1:8080)"
int sockToString(lua_State* L) {
    Socket& s = socketAt(L, 1);
    if (!s.open()) {
        lua_pushfstring(L, "net.Socket(%s, closed)", kindName(s.kind));
        return 1;
    }
    sockaddr_storage ss{};
    SockLen len = sizeof ss;
    std::string host;
    int port = 0;
    if (::getsockname(s.fd, reinterpret_cast<sockaddr*>(&ss), &len) == 0 &&
        addressText(reinterpret_cast<sockaddr*>(&ss), len, host, port)) {
        lua_pushfstring(L, "net.Socket(%s %s:%d)", kindName(s.kind), host.c_str(), port);
    } else {
        lua_pushfstring(L, "net.Socket(%s)", kindName(s.kind));
    }
    return 1;
}

// Creating sockets ----------------------------------------------------------------

struct ConnectSpec {
    ConnectTarget target;
    double timeout = -1;
    bool unix = false;
    std::string path;
    std::optional<Proxy> proxy;
    bool tls = false;
    TlsOptions tlsOptions;
    bool nodelay = false;
};

// Parses "socks5://user:pass@host:1080" or "http://host:3128".
bool parseProxyUrl(std::string_view url, Proxy& proxy, std::string& err) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        err = "proxy must look like socks5://host:port or http://host:port";
        return false;
    }
    proxy.type = lowercase(url.substr(0, scheme));
    if (proxy.type == "socks5h") {
        proxy.type = "socks5";
    }
    if (proxy.type != "socks5" && proxy.type != "http") {
        err = "proxy type must be socks5 or http";
        return false;
    }
    std::string_view rest = url.substr(scheme + 3);
    rest = rest.substr(0, rest.find('/'));
    if (const std::size_t at = rest.rfind('@'); at != std::string_view::npos) {
        const std::string_view user = rest.substr(0, at);
        const std::size_t colon = user.find(':');
        percentDecode(proxy.username, user.substr(0, colon), false);
        if (colon != std::string_view::npos) {
            percentDecode(proxy.password, user.substr(colon + 1), false);
        }
        rest.remove_prefix(at + 1);
    }
    std::size_t colon = rest.rfind(':');
    if (!rest.empty() && rest[0] == '[') {
        const std::size_t close = rest.find(']');
        if (close == std::string_view::npos) {
            err = "invalid proxy host";
            return false;
        }
        proxy.host = std::string(rest.substr(1, close - 1));
        colon = rest.find(':', close);
    } else {
        proxy.host = std::string(rest.substr(0, colon));
    }
    proxy.port = proxy.type == "socks5" ? 1080 : 8080;
    if (colon != std::string_view::npos) {
        proxy.port = std::atoi(std::string(rest.substr(colon + 1)).c_str());
    }
    if (proxy.host.empty() || proxy.port < 1 || proxy.port > 65535) {
        err = "invalid proxy host or port";
        return false;
    }
    return true;
}

// The connect job: everything that blocks, on a worker thread.
sched::Completion connectJob(const std::shared_ptr<ConnectSpec>& spec) {
    auto socket = std::make_shared<Socket>();
    socket->timeout = spec->timeout;
    std::string err;
    bool ok = false;

    if (spec->unix) {
#if SONATA_HAVE_UNIX_SOCKETS
        ok = openUnixConnect(*socket, spec->path, spec->timeout, err);
#else
        err = "Unix domain sockets are not available on this system";
#endif
    } else if (spec->proxy) {
        ConnectTarget proxyTarget;
        proxyTarget.host = spec->proxy->host;
        proxyTarget.port = spec->proxy->port;
        ok = openConnect(*socket, proxyTarget, spec->timeout, err);
        if (ok) {
            ok = spec->proxy->type == "socks5"
                     ? socks5Handshake(*socket, *spec->proxy, spec->target.host, spec->target.port, spec->timeout, err)
                     : httpConnectHandshake(*socket, *spec->proxy, spec->target.host, spec->target.port, spec->timeout, err);
            if (!ok) {
                err = "proxy: " + err;
            }
        }
    } else {
        ok = openConnect(*socket, spec->target, spec->timeout, err);
    }

    if (ok) {
        socket->host = spec->target.host;
        if (spec->nodelay) {
            setFlag(socket->fd, IPPROTO_TCP, TCP_NODELAY, true);
        }
    }
#ifdef SONATA_NET_TLS
    if (ok && spec->tls) {
        ok = handshake(*socket, spec->tlsOptions, err);
        if (!ok) {
            err = "TLS: " + err;
        }
    }
#else
    if (ok && spec->tls) {
        ok = false;
        err = "TLS is not available: build net with SONATA_NET_TLS and link OpenSSL";
    }
#endif
    if (!ok) {
        socket->close();
    }

    return [socket, ok, err](lua_State* T) {
        if (!ok) {
            return failure(T, err);
        }
        newSocketBox(T, socket);
        return 1;
    };
}

int familyOption(lua_State* L, int idx) {
    static const char* const kFamilies[] = {"any", "ipv4", "ipv6", nullptr};
    static const int kValues[] = {AF_UNSPEC, AF_INET, AF_INET6};
    return kValues[luaL_checkoption(L, idx, "any", kFamilies)];
}

// net.connect(host: string, port: number, options: number | ConnectOptions?): Socket | (nil, string)
int netConnect(lua_State* L) {
    auto spec = std::make_shared<ConnectSpec>();
    spec->target.host = luaL_checkstring(L, 1);
    spec->target.port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, spec->target.port >= 1 && spec->target.port <= 65535, 2, "port must be between 1 and 65535");

    if (lua_type(L, 3) == LUA_TNUMBER) {
        spec->timeout = optTimeout(L, 3);
    } else if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "timeout");
        spec->timeout = optTimeout(L, lua_gettop(L));
        lua_getfield(L, 3, "family");
        if (!lua_isnil(L, -1)) {
            spec->target.family = familyOption(L, lua_gettop(L));
        }
        lua_getfield(L, 3, "nodelay");
        spec->nodelay = lua_toboolean(L, -1) != 0;
        if (const auto local = optStringField(L, 3, "localAddress", "connect")) {
            spec->target.localAddress = std::string(*local);
        }
        lua_getfield(L, 3, "localPort");
        if (!lua_isnil(L, -1)) {
            spec->target.localPort = luaL_checkinteger(L, -1);
        }

        lua_getfield(L, 3, "proxy");
        if (lua_type(L, -1) == LUA_TSTRING) {
            Proxy proxy;
            std::string err;
            if (!parseProxyUrl(lua_tostring(L, -1), proxy, err)) {
                luaL_error(L, "connect: %s", err.c_str());
            }
            spec->proxy = proxy;
        } else if (lua_istable(L, -1)) {
            const int t = lua_gettop(L);
            Proxy proxy;
            proxy.type = lowercase(optStringField(L, t, "type", "connect").value_or("socks5"));
            proxy.host = std::string(optStringField(L, t, "host", "connect").value_or(""));
            lua_getfield(L, t, "port");
            proxy.port = lua_isnil(L, -1) ? (proxy.type == "http" ? 8080 : 1080) : luaL_checkinteger(L, -1);
            proxy.username = std::string(optStringField(L, t, "username", "connect").value_or(""));
            proxy.password = std::string(optStringField(L, t, "password", "connect").value_or(""));
            if ((proxy.type != "socks5" && proxy.type != "http") || proxy.host.empty()) {
                luaL_error(L, "connect: proxy needs type (\"socks5\" or \"http\") and host");
            }
            spec->proxy = proxy;
        } else if (!lua_isnil(L, -1)) {
            luaL_error(L, "connect: 'proxy' must be a URL string or a table");
        }

        lua_getfield(L, 3, "tls");
        if (!lua_isnil(L, -1) && !(lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1))) {
            spec->tls = true;
            spec->tlsOptions = readTlsOptions(L, lua_gettop(L), "connect");
        }
    }

    return Scheduler::of(L).await(L, [spec]() { return connectJob(spec); });
}

// Reads listen()/udp() options (a backlog number / reuse boolean, or a table).
BindOptions readBindOptions(lua_State* L, int idx, bool listen) {
    BindOptions o;
    o.backlog = listen ? 128 : 0;
    o.reuse = listen ? kReuseListen : false;
    if (lua_type(L, idx) == LUA_TNUMBER && listen) {
        o.backlog = luaL_checkinteger(L, idx);
    } else if (lua_type(L, idx) == LUA_TBOOLEAN && !listen) {
        o.reuse = lua_toboolean(L, idx) != 0;
    } else if (!lua_isnoneornil(L, idx)) {
        luaL_checktype(L, idx, LUA_TTABLE);
        lua_getfield(L, idx, "backlog");
        if (listen && !lua_isnil(L, -1)) {
            o.backlog = luaL_checkinteger(L, -1);
        }
        lua_getfield(L, idx, listen ? "reuseAddress" : "reuse");
        if (!lua_isnil(L, -1)) {
            o.reuse = lua_toboolean(L, -1) != 0;
        }
        lua_getfield(L, idx, "reusePort");
        o.reusePort = lua_toboolean(L, -1) != 0;
        lua_getfield(L, idx, "ipv6Only");
        o.v6only = lua_toboolean(L, -1) != 0;
        lua_getfield(L, idx, "family");
        if (!lua_isnil(L, -1)) {
            o.family = familyOption(L, lua_gettop(L));
        }
        lua_pop(L, 5);
    }
    luaL_argcheck(L, !listen || (o.backlog >= 1 && o.backlog <= 65535), idx, "backlog must be between 1 and 65535");
    return o;
}

// net.listen(host: string?, port: number, options: number | ListenOptions?): Socket | (nil, string)
int netListen(lua_State* L) {
    const char* host = luaL_optstring(L, 1, nullptr);
    const int port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, port >= 0 && port <= 65535, 2, "port must be between 0 and 65535");
    const BindOptions options = readBindOptions(L, 3, true);

    auto socket = std::make_shared<Socket>();
    std::string err;
    if (!openBound(*socket, host, port, options, err)) {
        return failure(L, err);
    }
    newSocketBox(L, std::move(socket));
    return 1;
}

// net.udp(host: string?, port: number?, options: boolean | UdpOptions?): Socket | (nil, string)
int netUdp(lua_State* L) {
    const char* host = luaL_optstring(L, 1, nullptr);
    const int port = luaL_optinteger(L, 2, 0);
    luaL_argcheck(L, port >= 0 && port <= 65535, 2, "port must be between 0 and 65535");
    const BindOptions options = readBindOptions(L, 3, false);

    bool broadcast = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "broadcast");
        broadcast = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    auto socket = std::make_shared<Socket>();
    socket->kind = Kind::Udp;
    std::string err;
    if (!openUdp(*socket, host, port, options, err)) {
        return failure(L, err);
    }
    if (broadcast) {
        setFlag(socket->fd, SOL_SOCKET, SO_BROADCAST, true);
    }
    newSocketBox(L, std::move(socket));
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

    auto socket = std::make_shared<Socket>();
    socket->kind = Kind::Raw;
    std::string err;
    if (!openRaw(*socket, family, protocol, err)) {
        return failure(L, err);
    }
    newSocketBox(L, std::move(socket));
    return 1;
}

// net.unix.connect(path: string, timeout: number?): Socket | (nil, string)
int unixConnect(lua_State* L) {
    auto spec = std::make_shared<ConnectSpec>();
    spec->unix = true;
    spec->path = luaL_checkstring(L, 1);
    spec->timeout = optTimeout(L, 2);
    return Scheduler::of(L).await(L, [spec]() { return connectJob(spec); });
}

// net.unix.listen(path: string, backlog: number?): Socket | (nil, string)
int unixListen(lua_State* L) {
    const std::string path = luaL_checkstring(L, 1);
    const int backlog = luaL_optinteger(L, 2, 128);
    luaL_argcheck(L, backlog >= 1 && backlog <= 65535, 2, "backlog must be between 1 and 65535");

#if SONATA_HAVE_UNIX_SOCKETS
    std::string err;
    sockaddr_un addr{};
    SockLen len = 0;
    if (!unixAddress(path, addr, len, err)) {
        return failure(L, err);
    }

    auto socket = std::make_shared<Socket>();
    socket->kind = Kind::Unix;
    socket->family = AF_UNIX;
    socket->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket->fd == kInvalid) {
        return failure(L, errorText(lastError()));
    }
    prepare(socket->fd);

    int rc = ::bind(socket->fd, reinterpret_cast<const sockaddr*>(&addr), len);
#ifndef _WIN32
    if (rc != 0 && lastError() == EADDRINUSE) {
        // A socket file left behind by a program that is gone? Then it is safe to replace.
        struct stat info {};
        const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        const bool stale = ::stat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) && probe >= 0 &&
                           ::connect(probe, reinterpret_cast<const sockaddr*>(&addr), len) != 0 && errno == ECONNREFUSED;
        if (probe >= 0) {
            ::close(probe);
        }
        if (stale && ::unlink(path.c_str()) == 0) {
            rc = ::bind(socket->fd, reinterpret_cast<const sockaddr*>(&addr), len);
        }
    }
#endif
    if (rc != 0 || ::listen(socket->fd, backlog) != 0) {
        return failure(L, errorText(lastError()));
    }
    socket->unixPath = path;
    newSocketBox(L, std::move(socket));
    return 1;
#else
    (void)path;
    return failure(L, "Unix domain sockets are not available on this system");
#endif
}

// net.select builds its result from the "revents" of every polled socket, packed
// two bytes each into a string. Stack: 1 readers, 2 writers, 3 timeout, 4 revents (or nil, 5 error)
int selectFinish(lua_State* L) {
    if (lua_type(L, 4) != LUA_TSTRING) {
        const std::string err = lua_isstring(L, 5) ? lua_tostring(L, 5) : "select failed";
        return failure(L, err);
    }
    std::size_t length = 0;
    const char* revents = lua_tolstring(L, 4, &length);
    const int readers = static_cast<int>(lua_objlen(L, 1));

    lua_createtable(L, 0, 0); // readable
    lua_createtable(L, 0, 0); // writable
    const int readable = lua_gettop(L) - 1;
    int outCount[2] = {0, 0};
    for (std::size_t k = 0; k * 2 + 1 < length; ++k) {
        const int t = k < static_cast<std::size_t>(readers) ? 0 : 1;
        const int index = t == 0 ? static_cast<int>(k) + 1 : static_cast<int>(k) - readers + 1;
        const auto ev = static_cast<short>((static_cast<unsigned char>(revents[k * 2]) << 8) | static_cast<unsigned char>(revents[k * 2 + 1]));
        lua_rawgeti(L, t + 1, index);
        const Socket* s = toSocket(L, -1);
        const bool ready = s == nullptr || s->fd == kInvalid || (t == 0 && hasBuffered(*s)) ||
                           (ev & (t == 0 ? POLLIN : POLLOUT)) || (ev & (POLLERR | POLLHUP | POLLNVAL));
        if (ready) {
            lua_rawseti(L, readable + t, ++outCount[t]);
        } else {
            lua_pop(L, 1);
        }
    }
    return 2;
}

int selectK(lua_State* L, int) {
    return selectFinish(L);
}

std::string packRevents(const std::vector<PollFd>& fds) {
    std::string packed;
    for (const PollFd& p : fds) {
        packed += static_cast<char>((static_cast<unsigned short>(p.revents) >> 8) & 0xFF);
        packed += static_cast<char>(static_cast<unsigned short>(p.revents) & 0xFF);
    }
    return packed;
}

// net.select(readers: { Socket }, writers: { Socket }?, timeout: number?): ({ Socket }, { Socket }) | (nil, string)
// A socket is "readable" when recv/accept would not wait (data buffered or arriving, or a closed peer).
int netSelect(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
    }
    const double timeout = optTimeout(L, 3);
    lua_settop(L, 3);
    if (!lua_istable(L, 2)) {
        lua_createtable(L, 0, 0);
        lua_replace(L, 2);
    }

    const int counts[2] = {static_cast<int>(lua_objlen(L, 1)), static_cast<int>(lua_objlen(L, 2))};

    std::vector<PollFd> fds;
    bool buffered = false;
    for (int t = 0; t < 2; ++t) {
        for (int i = 1; i <= counts[t]; ++i) {
            lua_rawgeti(L, t + 1, i);
            const Socket* s = toSocket(L, -1);
            if (s == nullptr) {
                luaL_error(L, "net.select: entry %d of argument #%d is not a socket", i, t + 1);
            }
            lua_pop(L, 1);

            PollFd p{};
            p.fd = s->fd;
            p.events = t == 0 ? POLLIN : POLLOUT;
            fds.push_back(p);
            buffered = buffered || (t == 0 && hasBuffered(*s));
        }
    }

    const int rc = fds.empty() ? 0 : pollSockets(fds.data(), fds.size(), 0);
    if (rc < 0 && !interrupted(lastError())) {
        return failure(L, errorText(lastError()));
    }
    if (rc > 0 || buffered || timeout == 0 || fds.empty()) {
        pushString(L, packRevents(fds));
        return selectFinish(L);
    }

    // Nothing ready yet: wait on a worker so other tasks can run meanwhile.
    const int r = Scheduler::of(L).await(L, [fds, timeout]() mutable -> sched::Completion {
        int result = 0;
        do {
            result = pollSockets(fds.data(), fds.size(), toMillis(timeout));
        } while (result < 0 && interrupted(lastError()));
        const std::string err = result < 0 ? errorText(lastError()) : std::string();
        const std::string packed = packRevents(fds);
        return [packed, err](lua_State* T) {
            if (!err.empty()) {
                return failure(T, err);
            }
            pushString(T, packed);
            return 1;
        };
    }, selectK);
    return r == sched::kInline ? selectFinish(L) : r;
}

#ifdef SONATA_NET_TLS
// net.tls.context(options: TlsOptions): TlsContext
int tlsContext(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const TlsOptions options = readTlsOptions(L, 1, "tls.context");
    luaL_argcheck(L, !options.context, 1, "options must not contain a context");
    luaL_argcheck(L, !options.server || !options.certFile.empty() || !options.certPem.empty(), 1,
                  "a server context needs a certificate (cert or certPem)");

    std::string err;
    TlsContextPtr context = makeContext(options, err);
    if (!context) {
        return failure(L, err);
    }
    void* memory = lua_newuserdatadtor(L, sizeof(TlsContextPtr), [](lua_State*, void* p) { static_cast<TlsContextPtr*>(p)->~TlsContextPtr(); });
    new (memory) TlsContextPtr(std::move(context));
    luaL_getmetatable(L, kTlsContextMeta);
    lua_setmetatable(L, -2);
    return 1;
}
#endif

struct Method {
    const char* name;
    int (*function)(lua_State*);
    sched::Continuation k;
};

const Method kSocketFunctions[] = {
    {"connect", netConnect, nullptr},
    {"listen", netListen, nullptr},
    {"udp", netUdp, nullptr},
    {"raw", netRaw, nullptr},
    {"select", netSelect, selectK},
};

const Function kUnixFunctions[] = {
    {"connect", unixConnect},
    {"listen", unixListen},
};

const Method kSocketMethods[] = {
    {"close", sockClose, nullptr},
    {"isClosed", sockIsClosed, nullptr},
    {"kind", sockKind, nullptr},
    {"setTimeout", sockSetTimeout, nullptr},
    {"getTimeout", sockGetTimeout, nullptr},
    {"send", sockSend, sendK},
    {"recv", sockRecv, recvK},
    {"recvUntil", sockRecvUntil, untilK},
    {"recvLine", sockRecvLine, untilK},
    {"recvAll", sockRecvAll, recvAllK},
    {"unread", sockUnread, nullptr},
    {"buffered", sockBuffered, nullptr},
    {"accept", sockAccept, acceptK},
    {"sendTo", sockSendTo, sendToK},
    {"recvFrom", sockRecvFrom, recvFromK},
    {"shutdown", sockShutdown, nullptr},
    {"setOption", sockSetOption, nullptr},
    {"getOption", sockGetOption, nullptr},
    {"localAddress", sockLocalAddress, nullptr},
    {"peerAddress", sockPeerAddress, nullptr},
    {"startTls", sockStartTls, nullptr},
    {"tlsInfo", sockTlsInfo, nullptr},
    {"peerCertificate", sockPeerCertificate, nullptr},
    {"joinGroup", sockJoinGroup, nullptr},
    {"leaveGroup", sockLeaveGroup, nullptr},
    {"setMulticastInterface", sockSetMulticastInterface, nullptr},
};

} // namespace

void pushSockets(lua_State* L) {
    ensureNetworking();

    if (luaL_newmetatable(L, kSocketMeta)) {
        lua_createtable(L, 0, static_cast<int>(std::size(kSocketMethods)));
        for (const Method& method : kSocketMethods) {
            lua_pushcclosurek(L, method.function, method.name, 0, method.k);
            lua_setfield(L, -2, method.name);
        }
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, sockToString, "Socket.__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "net.Socket");
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    for (const Method& f : kSocketFunctions) {
        lua_pushcclosurek(L, f.function, f.name, 0, f.k);
        lua_setfield(L, -2, f.name);
    }

    lua_createtable(L, 0, static_cast<int>(std::size(kUnixFunctions)) + 1);
    setFunctionList(L, kUnixFunctions);
    lua_setfield(L, -2, "unix");
}

void pushTls(lua_State* L) {
    lua_createtable(L, 0, 2);
#ifdef SONATA_NET_TLS
    registerType(L, kTlsContextMeta, "net.TlsContext", static_cast<const Function*>(nullptr), 0);
    lua_pushcfunction(L, tlsContext, "tls.context");
    lua_setfield(L, -2, "context");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "available");
    lua_pushstring(L, OpenSSL_version(OPENSSL_VERSION));
    lua_setfield(L, -2, "library");
#else
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "available");
#endif
    lua_setreadonly(L, -1, 1);
}

} // namespace sonata::lib::libs::net
