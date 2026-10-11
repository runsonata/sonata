// @sonata/net: networking building blocks: sockets, TLS, DNS, addresses, URLs, and the
// wire formats of HTTP/1.1 and WebSocket. Everything a protocol library (an HTTP
// client or server, a WebSocket library, ...) needs, without being one itself.
//
//     local net = require("@sonata/net")
//
// Failures that depend on the outside world (refused, timed out, DNS...) do NOT
// raise: the call returns nil, "message". Only misuse (wrong argument types,
// using a closed socket) raises. Waiting (connect, recv, accept, DNS, ...) never
// blocks other tasks (see @sonata/task); outside a task it waits right there.
// Sockets close themselves when garbage collected.
//
// Sockets
//     net.connect(host, port, options?)        --> Socket   TCP client; options: a timeout (seconds), or
//         { timeout, family = "any" | "ipv4" | "ipv6", nodelay, localAddress, localPort,
//           tls = true | TlsOptions | TlsContext,               -- handshake right after connecting
//           proxy = "socks5://user:pass@host:1080" | "http://host:3128" | { type, host, port, username, password } }
//         The timeout applies to the connection, and then to every later wait of the socket.
//     net.listen(host?, port, options?)        --> Socket   TCP server (host nil = all interfaces, port 0 = any);
//         options: a backlog, or { backlog, reuseAddress, reusePort, ipv6Only, family }
//     net.udp(host?, port?, options?)          --> Socket   UDP (bound if host or port is given);
//         options: reuse (boolean), or { reuse, reusePort, broadcast, family }
//     net.raw(protocol, family?)               --> Socket   raw IP socket: "icmp" | "icmpv6" | "tcp" | "udp" | 0-255,
//                                                           family "ipv4" | "ipv6" (needs root / CAP_NET_RAW)
//     net.unix.connect(path, timeout?)         --> Socket   Unix domain socket client
//     net.unix.listen(path, backlog?)          --> Socket   Unix domain socket server (replaces a stale socket file;
//                                                           the file is removed when the server closes)
//     net.select(readers, writers?, timeout?)  --> { ready to read }, { ready to write }
//
//     sock:send(data)              --> bytesSent     sends everything, or nil, err, bytesSentSoFar
//     sock:recv(max?, exact?)      --> string        up to max bytes (default 4096); exact waits for all of them;
//                                                    "" means the peer closed the connection
//     sock:recvUntil(delim, max?)  --> string        includes the delimiter; max defaults to 1 MiB
//     sock:recvLine(max?)          --> string        one line, without "\n" / "\r\n" (the last line may lack one)
//     sock:recvAll(max?)           --> string        everything until the peer closes
//     sock:unread(data)                              puts data back in front of what recv() returns next
//     sock:buffered()              --> number        bytes received but not read yet
//     sock:accept(timeout?)        --> Socket        (server sockets)
//     sock:sendTo(data, host, port?) --> bytesSent   (UDP, or raw: no port)
//     sock:recvFrom(max?)          --> data, host, port   (UDP/raw, max defaults to 2048)
//     sock:startTls(options?)      --> true          upgrade to TLS (client, or server on an accepted socket)
//     sock:tlsInfo()               --> { version, cipher, alpn?, serverName? } | nil
//     sock:peerCertificate()       --> { subject, issuer, serial, notBefore, notAfter, fingerprint (SHA-256 hex),
//                                        altNames } | nil
//     sock:joinGroup(group, iface?), sock:leaveGroup(group, iface?), sock:setMulticastInterface(iface)
//     sock:shutdown(how?)          --> true          "write" (default) | "read" | "both"
//     sock:setTimeout(seconds?)    nil = wait forever (default); applies to every wait
//     sock:getTimeout()            --> number?
//     sock:setOption(name, value), sock:getOption(name)
//         booleans: "nodelay" "keepalive" "broadcast" "reuseAddress" "hdrincl" "multicastLoop"
//         numbers: "recvBufferSize" "sendBufferSize" "ttl" "multicastTtl"; "linger": seconds or false
//     sock:localAddress(), sock:peerAddress()  --> host, port
//     sock:kind()                  --> "tcp" | "udp" | "raw" | "unix"
//     sock:close(), sock:isClosed()
//
//   One task reading and one writing at the same time is fine; two tasks reading the same
//   socket get the data in whatever order they happen to run.
//
// TLS (OpenSSL)
//     TlsOptions = { host (name for SNI and verification; default: the connect() host),
//                    verify = true, caFile | caPem (default: the system's trust store),
//                    cert | certPem, key | keyPem (our certificate: servers, or clients for mutual TLS),
//                    alpn = { "h2", "http/1.1" }, minVersion = "1.2" | "1.3",
//                    server (contexts only), verifyClient (servers: require a client certificate),
//                    context = TlsContext }
//     net.tls.context(options)     --> TlsContext    a configuration loaded once and shared by many connections
//                                                    (servers should use one); pass it as tls / startTls option
//     net.tls.available            --> boolean       net.tls.library --> "OpenSSL 3.x ..."
//   TLS 1.2 or newer only. verify = false disables ALL certificate checks: use it for testing only.
//   Mid-stream upgrades (SMTP/IMAP STARTTLS) work, but startTls refuses if unread data is buffered.
//
// DNS
//     net.dns.resolve(host, family?)           --> { "93.184.216.34", "2606:2800:..." }  (the system resolver)
//     net.resolve(host, family?)               the same
//     net.dns.reverse(address)                 --> "host.example.com"
//     net.dns.query(name, type?, options?)     --> { Record }   asks a DNS server directly
//         type: "A" (default) "AAAA" "CNAME" "MX" "NS" "PTR" "SOA" "SRV" "TXT" "CAA" "HTTPS" ... or a number
//         options: server ("1.1.1.1", "[::1]:53" or a list; default: the system's), timeout (5), tcp (false)
//         Record = { name, type, ttl, ... } with address (A/AAAA), target (CNAME/NS/PTR/SRV),
//                  priority + exchange (MX), text + strings (TXT), priority/weight/port (SRV),
//                  mname/rname/serial/refresh/retry/expire/minimum (SOA), flags/tag/value (CAA), data (others)
//     net.dns.servers()                        --> { string }   the system's DNS servers
//
// Addresses
//     net.ip.isValid(text), net.ip.version(text) --> 4 | 6 | nil
//     net.ip.parse(text)                       --> { version, bytes, zone? }
//     net.ip.normalize(text)                   --> canonical form ("::1", "2001:db8::1")
//     net.ip.fromBytes(bytes)                  --> text (4 or 16 bytes)
//     net.ip.kind(text)                        --> "public" | "private" | "loopback" | "linkLocal" | "multicast" |
//                                                  "unspecified" | "broadcast" | "documentation" | "reserved" | ...
//     net.ip.isPublic(text)                    --> boolean
//     net.ip.inRange(address, cidr | { cidr }) --> boolean
//     net.ip.parseCidr("10.0.0.0/8")           --> { version, prefix, network, first, last, size, mask? }
//     net.interfaces()                         --> { { name, address, family, netmask, prefix, internal, mac? } }
//
// URLs
//     net.url.parse("https://u:p@Host:8443/a/b?x=1#top")
//         --> { scheme="https", username="u", password="p", host="host", port=8443,
//               path="/a/b", query="x=1", fragment="top" }     (absent parts are nil; not decoded)
//     net.url.build({ scheme="https", host="a.com", path="/s", query={ q="a b" } })  --> "https://a.com/s?q=a%20b"
//     net.url.resolve(base, reference)             --> absolute URL (RFC 3986: "../x", "//host/y", "?q" ...)
//     net.url.normalize(url)                       --> lower-case scheme and host, no default port, no "." / ".."
//     net.url.origin(url)                          --> "https://example.com:8443"
//     net.url.defaultPort(scheme)                  --> 443 for "https", ...
//     net.url.encode("a b/c", "/")                 --> "a%20b/c"       second arg: extra characters to keep
//     net.url.decode("a%20b+c", true)              --> "a b c"         true: '+' means space
//     net.url.parseQuery("a=1&b=2&a=3")            --> { a="1", b="2" }, { {"a","1"}, {"b","2"}, {"a","3"} }
//     net.url.buildQuery({ {"a","1"}, {"a","2"} }) --> "a=1&a=2"       (a { k = v | { v } } map works too, sorted)
//     net.idna.toAscii("bücher.de")                --> "xn--bcher-kva.de"   (punycode; no Unicode normalization)
//     net.idna.toUnicode("xn--bcher-kva.de")       --> "bücher.de"
//
// HTTP/1.1 (no I/O: bytes in, tables out, and the reverse)
//     net.http.parseRequest(data, limits?)     --> head, bytesUsed | nil (incomplete) | nil, err
//         head = { method, target, path, query?, version = "1.1", headers, headerList }
//         headers: lower-case names; repeated headers joined with ", " (Set-Cookie with "\n")
//         headerList: { { name, value } } exactly as received.  limits: { maxHeadSize = 64 KiB, maxHeaders = 100 }
//     net.http.parseResponse(data, limits?)    --> head = { version, status, reason, headers, headerList }, bytesUsed
//     net.http.formatRequest({ method, target, version?, headers? })    --> the head, ending in a blank line
//     net.http.formatResponse({ status, reason?, version?, headers? })  --> the head
//         headers: { name = value | { value } } or { { name, value } }; line breaks in values are refused
//     net.http.bodyLength(head, { requestMethod? }?)
//         --> "none" | "length", n | "chunked" | "close" (read until EOF) | "tunnel" | nil, err   (RFC 9112 6.3)
//     net.http.keepAlive(head)                 --> boolean     does the connection stay open afterwards?
//     net.http.statusText(404)                 --> "Not Found"
//     net.http.encodeChunk(data), net.http.lastChunk(trailers?)   chunked transfer coding
//     net.http.chunkedDecoder(options?)        --> decoder
//         decoder:push(bytes) --> bodyBytes, done | nil, err      decoder:rest() --> bytes after the body
//         decoder:trailers()  --> { [name]: value }               decoder:done()
//     net.http.formatDate(time?), net.http.parseDate(text)        "Sun, 06 Nov 1994 08:49:37 GMT" <-> Unix time
//     net.http.parseHeaderValue("text/html; charset=utf-8") --> "text/html", { charset = "utf-8" }
//     net.http.parseList("a, \"b,c\", d")      --> { "a", "\"b,c\"", "d" }
//     net.http.parseQuality("br;q=1, gzip;q=0.8, *;q=0.1") --> { { value = "br", q = 1, params = {} }, ... }
//     net.http.basicAuth(user, password)       --> "Basic ..."   net.http.parseBasicAuth(header) --> user, password
//     net.http.parseCookies("a=1; b=2")        --> { a = "1", b = "2" }
//     net.http.parseSetCookie(header)          --> { name, value, expires?, maxAge?, domain?, path?, secure,
//                                                    httpOnly, sameSite?, partitioned? }
//     net.http.formatSetCookie(cookie), net.http.formatCookies({ a = "1" })
//     net.http.encodeMultipart(parts, boundary?) --> body, contentType
//         parts = { { name, value, filename?, contentType?, headers? } }
//     net.http.decodeMultipart(body, boundaryOrContentType) --> { { name?, filename?, contentType?, headers, value } }
//     net.mime.lookup("photo.png")             --> "image/png"    (also "png")
//     net.mime.extension("image/png")          --> "png"
//     net.mime.parse("text/html; charset=utf-8") --> { type, subtype, essence, params }
//     net.mime.isText(type)                    --> boolean
//
// WebSocket (RFC 6455 framing; no I/O)
//     net.ws.generateKey()                     --> a Sec-WebSocket-Key
//     net.ws.acceptKey(key)                    --> the matching Sec-WebSocket-Accept
//     net.ws.encode(opcode, payload?, { mask, fin, rsv1 }?) --> frame bytes
//         opcode: "text" "binary" "close" "ping" "pong" "continuation" or 0-15; mask = true (clients
//         must mask) or a 4-byte key
//     net.ws.decode(data, { maxPayload }?)     --> frame, bytesUsed | nil (incomplete) | nil, err
//         frame = { opcode, fin, rsv1, rsv2, rsv3, masked, payload (unmasked) }
//     net.ws.closePayload(code?, reason?), net.ws.parseClose(payload) --> code?, reason
//     net.ws.mask(data, key)                   --> data XOR key

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"
#include "net/common.hpp"

namespace sonata::lib::libs {

void openNet(lua_State* L) {
    net::ensureNetworking();

    lua_createtable(L, 0, 16);
    net::pushSockets(L);
    net::pushInterfaces(L);

    net::pushDns(L);
    lua_getfield(L, -1, "resolve");
    lua_setfield(L, -3, "resolve");
    lua_setfield(L, -2, "dns");

    net::pushIp(L);
    lua_setfield(L, -2, "ip");
    net::pushUrl(L);
    lua_setfield(L, -2, "url");
    net::pushIdna(L);
    lua_setfield(L, -2, "idna");
    net::pushHttp(L);
    lua_setfield(L, -2, "http");
    net::pushMime(L);
    lua_setfield(L, -2, "mime");
    net::pushWs(L);
    lua_setfield(L, -2, "ws");
    net::pushTls(L);
    lua_setfield(L, -2, "tls");
}

} // namespace sonata::lib::libs
