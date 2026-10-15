--[[
	Type definitions for @sonata/net, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Networking building blocks: sockets, TLS, DNS, IP addresses, URLs, and the wire formats of
	HTTP/1.1 and WebSocket.

	Failures that depend on the outside world (connection refused, timeouts, DNS errors, ...) don't
	raise: the call returns `nil, message`. Only misuse raises. Waiting never blocks other tasks.
]=]
local net = {}

export type Family = "any" | "ipv4" | "ipv6"

--- Made by `net.tls.context`.
export type TlsContext = { __type: "net.TlsContext" }

export type TlsOptions = {
	--- Name for SNI and verification (default: the host given to connect).
	host: string?,
	--- Default `true`. `false` disables all certificate checks: use it for testing only.
	verify: boolean?,
	--- Trusted CAs, as a file (default: the system's trust store).
	caFile: string?,
	--- Trusted CAs, as PEM text.
	caPem: string?,
	--- Our certificate file: servers, or clients for mutual TLS.
	cert: string?,
	--- Our certificate, as PEM text.
	certPem: string?,
	--- The certificate's private key file.
	key: string?,
	--- The private key, as PEM text.
	keyPem: string?,
	--- e.g. `{ "h2", "http/1.1" }`
	alpn: { string }?,
	minVersion: ("1.2" | "1.3")?,
	--- Contexts only: a server context.
	server: boolean?,
	--- Servers: require a client certificate.
	verifyClient: boolean?,
	context: TlsContext?,
}

export type ProxyOptions = {
	type: string,
	host: string,
	port: number,
	username: string?,
	password: string?,
}

export type ConnectOptions = {
	--- Seconds. Applies to the connection, and then to every later wait on the socket.
	timeout: number?,
	family: Family?,
	--- Disable Nagle's algorithm.
	nodelay: boolean?,
	localAddress: string?,
	localPort: number?,
	--- TLS handshake right after connecting.
	tls: (boolean | TlsOptions | TlsContext)?,
	--- A URL (`"socks5://user:pass@host:1080"`, `"http://host:3128"`) or a table.
	proxy: (string | ProxyOptions)?,
}

export type ListenOptions = {
	backlog: number?,
	reuseAddress: boolean?,
	reusePort: boolean?,
	ipv6Only: boolean?,
	family: Family?,
}

export type UdpOptions = {
	reuse: boolean?,
	reusePort: boolean?,
	broadcast: boolean?,
	family: Family?,
}

export type TlsInfo = {
	version: string,
	cipher: string,
	alpn: string?,
	serverName: string?,
}

export type Certificate = {
	subject: string,
	issuer: string,
	serial: string,
	notBefore: number,
	notAfter: number,
	--- SHA-256, hex.
	fingerprint: string,
	altNames: { string },
}

export type SocketOption =
	"nodelay"
	| "keepalive"
	| "broadcast"
	| "reuseAddress"
	| "hdrincl"
	| "multicastLoop"
	| "recvBufferSize"
	| "sendBufferSize"
	| "ttl"
	| "multicastTtl"
	| "linger"

export type Socket = {
	--- Sends everything. Returns the bytes sent, or `nil, err, bytesSentSoFar`.
	send: (self: Socket, data: string) -> (number?, string?, number?),
	--- Up to `max` bytes (default 4096). With `exact`, waits for all of them. `""` means the peer closed the connection.
	recv: (self: Socket, max: number?, exact: boolean?) -> (string?, string?),
	--- Everything up to and including `delimiter` (`max` defaults to 1 MiB).
	recvUntil: (self: Socket, delimiter: string, max: number?) -> (string?, string?),
	--- One line, without `"\n"` or `"\r\n"`. The last line may lack one.
	recvLine: (self: Socket, max: number?) -> (string?, string?),
	--- Everything until the peer closes.
	recvAll: (self: Socket, max: number?) -> (string?, string?),
	--- Puts data back in front of what `recv` returns next.
	unread: (self: Socket, data: string) -> (),
	--- Bytes received but not read yet.
	buffered: (self: Socket) -> number,
	--- Accepts a connection (server sockets).
	accept: (self: Socket, timeout: number?) -> (Socket?, string?),
	--- UDP, or raw (no port). Returns the bytes sent.
	sendTo: (self: Socket, data: string, host: string, port: number?) -> (number?, string?),
	--- UDP or raw (`max` defaults to 2048). Returns `data, host, port`.
	recvFrom: (self: Socket, max: number?) -> (string?, string?, number?),
	--- Upgrades to TLS: as a client, or as a server on an accepted socket. Refuses if unread data is buffered.
	startTls: (self: Socket, options: TlsOptions?) -> (boolean?, string?),
	--- Details of the TLS session, or `nil`.
	tlsInfo: (self: Socket) -> TlsInfo?,
	--- The peer's certificate, or `nil`.
	peerCertificate: (self: Socket) -> Certificate?,
	joinGroup: (self: Socket, group: string, interface: string?) -> (boolean?, string?),
	leaveGroup: (self: Socket, group: string, interface: string?) -> (boolean?, string?),
	setMulticastInterface: (self: Socket, interface: string) -> (boolean?, string?),
	--- `"write"` (default), `"read"` or `"both"`.
	shutdown: (self: Socket, how: ("write" | "read" | "both")?) -> (boolean?, string?),
	--- Timeout for every later wait; `nil` waits forever (the default).
	setTimeout: (self: Socket, seconds: number?) -> (),
	getTimeout: (self: Socket) -> number?,
	--- `"linger"` takes seconds, or `false`.
	setOption: (self: Socket, name: SocketOption, value: boolean | number) -> (boolean?, string?),
	getOption: (self: Socket, name: SocketOption) -> (boolean | number | nil, string?),
	--- `host, port`
	localAddress: (self: Socket) -> (string?, number | string | nil),
	--- `host, port`
	peerAddress: (self: Socket) -> (string?, number | string | nil),
	kind: (self: Socket) -> "tcp" | "udp" | "raw" | "unix",
	close: (self: Socket) -> (),
	isClosed: (self: Socket) -> boolean,
}

--- `name`, `type` and `ttl`, plus the fields of its record type.
export type DnsRecord = {
	name: string,
	type: string,
	ttl: number,
	--- A, AAAA
	address: string?,
	--- CNAME, NS, PTR, SRV
	target: string?,
	--- MX, SRV
	priority: number?,
	--- MX
	exchange: string?,
	--- TXT (joined)
	text: string?,
	--- TXT
	strings: { string }?,
	--- SRV
	weight: number?,
	--- SRV
	port: number?,
	--- SOA
	mname: string?,
	rname: string?,
	serial: number?,
	refresh: number?,
	retry: number?,
	expire: number?,
	minimum: number?,
	--- CAA
	flags: number?,
	tag: string?,
	value: string?,
	--- Other types: raw bytes.
	data: string?,
}

export type QueryOptions = {
	--- `"1.1.1.1"`, `"[::1]:53"` or a list (default: the system's).
	server: (string | { string })?,
	--- Seconds (default 5).
	timeout: number?,
	--- Use TCP (default `false`).
	tcp: boolean?,
}

export type Interface = {
	name: string,
	address: string,
	family: string,
	netmask: string,
	prefix: number,
	internal: boolean,
	mac: string?,
}

export type Url = {
	scheme: string?,
	username: string?,
	password: string?,
	host: string?,
	port: number?,
	path: string?,
	--- A string, or (for `build`) a table as for `buildQuery`.
	query: (string | { [any]: any })?,
	fragment: string?,
}

--- `{ name = value | { value } }` or `{ { name, value } }`.
export type Headers = { [string]: string | { string } } | { { string } }

export type HeadLimits = {
	--- Default 64 KiB.
	maxHeadSize: number?,
	--- Default 100.
	maxHeaders: number?,
}

export type RequestHead = {
	method: string,
	target: string,
	path: string,
	query: string?,
	version: string,
	--- Lower-case names; repeated headers joined by `", "` (`set-cookie` by `"\n"`).
	headers: { [string]: string },
	--- `{ { name, value } }` exactly as received.
	headerList: { { string } },
}

export type ResponseHead = {
	version: string,
	status: number,
	reason: string,
	--- Lower-case names; repeated headers joined by `", "` (`set-cookie` by `"\n"`).
	headers: { [string]: string },
	--- `{ { name, value } }` exactly as received.
	headerList: { { string } },
}

export type ChunkedDecoder = {
	--- Returns the body bytes decoded so far and whether the body ended, or `nil, err`.
	push: (self: ChunkedDecoder, data: string) -> (string?, boolean | string),
	--- The bytes after the end of the body (the next message), taken out.
	rest: (self: ChunkedDecoder) -> string,
	trailers: (self: ChunkedDecoder) -> { [string]: string },
	done: (self: ChunkedDecoder) -> boolean,
}

export type Cookie = {
	name: string,
	value: string,
	expires: number?,
	maxAge: number?,
	domain: string?,
	path: string?,
	secure: boolean?,
	httpOnly: boolean?,
	sameSite: string?,
	partitioned: boolean?,
}

export type Part = {
	name: string?,
	value: string,
	filename: string?,
	contentType: string?,
	headers: { [string]: string }?,
}

export type MediaType = {
	type: string,
	subtype: string,
	--- `type/subtype`
	essence: string,
	params: { [string]: string },
}

export type Opcode = "text" | "binary" | "close" | "ping" | "pong" | "continuation" | number

export type Frame = {
	opcode: string | number,
	fin: boolean,
	rsv1: boolean,
	rsv2: boolean,
	rsv3: boolean,
	masked: boolean,
	--- Unmasked.
	payload: string,
}

--[=[
	Opens a TCP connection. `options` may be just a timeout.

	```luau
	local sock = assert(net.connect("example.com", 443, { tls = true, timeout = 10 }))
	```
]=]
function net.connect(host: string, port: number, options: (number | ConnectOptions)?): (Socket?, string?)
	return nil :: any
end

--[=[
	Opens a TCP server. `host = nil` listens on all interfaces; `port = 0` picks a free port (read
	it back with `sock:localAddress()`). `options` may be just a backlog.
]=]
function net.listen(host: string?, port: number, options: (number | ListenOptions)?): (Socket?, string?)
	return nil :: any
end

--- Opens a UDP socket, bound if `host` or `port` is given. `options` may be just `reuse`.
function net.udp(host: string?, port: number?, options: (boolean | UdpOptions)?): (Socket?, string?)
	return nil :: any
end

--- Opens a raw IP socket for `"icmp"`, `"icmpv6"`, `"tcp"`, `"udp"` or a protocol number. Needs administrator rights.
function net.raw(protocol: string | number, family: ("ipv4" | "ipv6")?): (Socket?, string?)
	return nil :: any
end

--- Waits until some of the sockets are ready, and returns those ready to read and those ready to write.
function net.select(readers: { Socket }, writers: { Socket }?, timeout: number?): ({ Socket }, { Socket } | string)
	return nil :: any, nil :: any
end

--- The addresses of `host`, through the system resolver. Same as `net.dns.resolve`.
function net.resolve(host: string, family: Family?): ({ string }?, string?)
	return nil :: any
end

--- The machine's network interfaces and their addresses.
function net.interfaces(): { Interface }
	return nil :: any
end

--- Unix domain sockets.
net.unix = {}

function net.unix.connect(path: string, timeout: number?): (Socket?, string?)
	return nil :: any
end

--- Replaces a stale socket file, and removes the file when the server closes.
function net.unix.listen(path: string, backlog: number?): (Socket?, string?)
	return nil :: any
end

--- TLS (OpenSSL), TLS 1.2 and newer.
net.tls = {}

--- `true` when TLS is supported.
net.tls.available = (nil :: any) :: boolean

--- The TLS library, e.g. `"OpenSSL 3.x ..."`. `nil` when TLS isn't available.
net.tls.library = (nil :: any) :: string?

--- A TLS configuration loaded once and shared by many connections. Servers should use one.
function net.tls.context(options: TlsOptions): (TlsContext?, string?)
	return nil :: any
end

--- DNS lookups. Every lookup runs on a worker thread.
net.dns = {}

--- The addresses of `host`, through the system resolver.
function net.dns.resolve(host: string, family: Family?): ({ string }?, string?)
	return nil :: any
end

--- The host name of an address.
function net.dns.reverse(address: string): (string?, string?)
	return nil :: any
end

--[=[
	Asks a DNS server directly. `type` is `"A"` (default), `"AAAA"`, `"CNAME"`, `"MX"`, `"NS"`,
	`"PTR"`, `"SOA"`, `"SRV"`, `"TXT"`, `"CAA"`, `"HTTPS"`, ... or a number.
]=]
function net.dns.query(name: string, type: (string | number)?, options: QueryOptions?): ({ DnsRecord }?, string?)
	return nil :: any
end

--- The system's DNS servers.
function net.dns.servers(): { string }
	return nil :: any
end

--- IP addresses.
net.ip = {}

function net.ip.isValid(text: string): boolean
	return nil :: any
end

--- 4, 6 or `nil`.
function net.ip.version(text: string): number?
	return nil :: any
end

function net.ip.parse(text: string): ({ version: number, bytes: string, zone: string? }?, string?)
	return nil :: any
end

--- The canonical form: `"::1"`, `"2001:db8::1"`.
function net.ip.normalize(text: string): (string?, string?)
	return nil :: any
end

--- The text form of 4 or 16 raw bytes.
function net.ip.fromBytes(bytes: string): string
	return nil :: any
end

--- `"public"`, `"private"`, `"loopback"`, `"linkLocal"`, `"multicast"`, `"unspecified"`, `"broadcast"`, `"documentation"`, `"reserved"`, ...
function net.ip.kind(text: string): string
	return nil :: any
end

--- Whether the address is routable on the internet.
function net.ip.isPublic(text: string): boolean
	return nil :: any
end

--- Whether the address is in a CIDR range; `ranges` is one CIDR string or a list of them.
function net.ip.inRange(address: string, ranges: string | { string }): boolean
	return nil :: any
end

function net.ip.parseCidr(
	cidr: string
): (
	{
		version: number,
		prefix: number,
		network: string,
		first: string,
		last: string,
		size: number,
		mask: string?,
	}?,
	string?
)
	return nil :: any
end

--- URLs (RFC 3986).
net.url = {}

--- Splits a URL. Absent parts are `nil`. Parts are not percent-decoded.
function net.url.parse(url: string): (Url?, string?)
	return nil :: any
end

--- Builds a URL. `query` may be a string or a table (as for `buildQuery`).
function net.url.build(parts: Url): string
	return nil :: any
end

--- The absolute URL of a reference relative to `base` (`"../x"`, `"//host/y"`, `"?q"`, ...).
function net.url.resolve(base: string, reference: string): (string?, string?)
	return nil :: any
end

--- Lower-case scheme and host, no default port, no `.`/`..` segments.
function net.url.normalize(url: string): (string?, string?)
	return nil :: any
end

--- `"https://example.com:8443"`
function net.url.origin(url: string): (string?, string?)
	return nil :: any
end

--- 443 for `"https"`, ...
function net.url.defaultPort(scheme: string): number?
	return nil :: any
end

--- Percent-encodes `text`; `keep` lists extra characters to leave alone.
function net.url.encode(text: string, keep: string?): string
	return nil :: any
end

--- Percent-decodes; with `plusIsSpace`, `+` means space.
function net.url.decode(text: string, plusIsSpace: boolean?): (string?, string?)
	return nil :: any
end

--- Returns a map (`{ a = "1" }`) and the ordered list of pairs (`{ { "a", "1" } }`).
function net.url.parseQuery(query: string): ({ [string]: string }?, { { string } } | string)
	return nil :: any, nil :: any
end

--- From a list of pairs `{ { "a", "1" } }`, or a map whose values are strings or lists of strings (sorted).
function net.url.buildQuery(params: { { string } } | { [string]: string | number | boolean | { string } }): string
	return nil :: any
end

--- Internationalized domain names.
net.idna = {}

--- `"bücher.de"` to `"xn--bcher-kva.de"` (punycode; no Unicode normalization).
function net.idna.toAscii(domain: string): (string?, string?)
	return nil :: any
end

--- `"xn--bcher-kva.de"` to `"bücher.de"`.
function net.idna.toUnicode(domain: string): (string?, string?)
	return nil :: any
end

--- HTTP/1.1 wire format. No I/O happens here.
net.http = {}

--[=[
	Parses a request head at the beginning of `data`. Returns `head, bytesUsed`; `nil` if the head
	isn't complete yet; or `nil, err` if it is invalid.
]=]
function net.http.parseRequest(data: string, limits: HeadLimits?): (RequestHead?, number | string | nil)
	return nil :: any
end

--[=[
	Parses a response head at the beginning of `data`. Returns `head, bytesUsed`; `nil` if the head
	isn't complete yet; or `nil, err` if it is invalid.
]=]
function net.http.parseResponse(data: string, limits: HeadLimits?): (ResponseHead?, number | string | nil)
	return nil :: any
end

--- Produces a request head, ending in the blank line.
function net.http.formatRequest(head: { method: string, target: string, version: string?, headers: Headers? }): string
	return nil :: any
end

--- Produces a response head, ending in the blank line.
function net.http.formatResponse(head: { status: number, reason: string?, version: string?, headers: Headers? }): string
	return nil :: any
end

--[=[
	How the body is delimited (RFC 9112 §6.3): `"none"`, `"length", n`, `"chunked"`, `"close"`
	(read until EOF), `"tunnel"`, or `nil, err`. Give `requestMethod` for responses.
]=]
function net.http.bodyLength(
	head: RequestHead | ResponseHead,
	options: { requestMethod: string? }?
): (string?, number | string | nil)
	return nil :: any
end

--- Whether the connection stays open after this message.
function net.http.keepAlive(head: RequestHead | ResponseHead): boolean
	return nil :: any
end

--- `statusText(404)` gives `"Not Found"`.
function net.http.statusText(code: number): string?
	return nil :: any
end

--- One chunk of a chunked body (`""` for empty data).
function net.http.encodeChunk(data: string): string
	return nil :: any
end

--- The final chunk, with optional trailers.
function net.http.lastChunk(trailers: Headers?): string
	return nil :: any
end

--- A decoder for a chunked body.
function net.http.chunkedDecoder(options: { maxTrailerSize: number? }?): ChunkedDecoder
	return nil :: any
end

--- Unix time (default: now) as `"Sun, 06 Nov 1994 08:49:37 GMT"`.
function net.http.formatDate(time: number?): string
	return nil :: any
end

--- An HTTP date as Unix time.
function net.http.parseDate(text: string): number?
	return nil :: any
end

--- `"text/html; charset=utf-8"` gives `"text/html", { charset = "utf-8" }`.
function net.http.parseHeaderValue(value: string): (string, { [string]: string })
	return nil :: any, nil :: any
end

--- A comma-separated list, respecting quotes.
function net.http.parseList(value: string): { string }
	return nil :: any
end

--- `"br;q=1, gzip;q=0.8"` gives `{ { value = "br", q = 1, params = {} }, ... }`.
function net.http.parseQuality(value: string): { { value: string, q: number, params: { [string]: string } } }
	return nil :: any
end

--- `"Basic ..."`
function net.http.basicAuth(username: string, password: string): string
	return nil :: any
end

--- `user, password`, or `nil`.
function net.http.parseBasicAuth(header: string): (string?, string?)
	return nil :: any
end

--- `"a=1; b=2"` gives `{ a = "1", b = "2" }`.
function net.http.parseCookies(header: string): { [string]: string }
	return nil :: any
end

--- The reverse of `parseCookies`.
function net.http.formatCookies(cookies: { [string]: string } | { { string } }): string
	return nil :: any
end

--- Parses a Set-Cookie header.
function net.http.parseSetCookie(header: string): (Cookie?, string?)
	return nil :: any
end

--- Produces a Set-Cookie header value.
function net.http.formatSetCookie(cookie: Cookie): string
	return nil :: any
end

--- Returns the body and the matching Content-Type header value.
function net.http.encodeMultipart(parts: { Part }, boundary: string?): (string, string)
	return nil :: any, nil :: any
end

function net.http.decodeMultipart(body: string, boundaryOrContentType: string): ({ Part }?, string?)
	return nil :: any
end

--- Media types.
net.mime = {}

--- `lookup("photo.png")` or `lookup("png")` gives `"image/png"`.
function net.mime.lookup(nameOrExt: string): string?
	return nil :: any
end

--- `extension("image/png")` gives `"png"`.
function net.mime.extension(type: string): string?
	return nil :: any
end

function net.mime.parse(type: string): (MediaType?, string?)
	return nil :: any
end

--- Whether the type is textual (text/*, JSON, XML, JavaScript, ...).
function net.mime.isText(type: string): boolean
	return nil :: any
end

--- WebSocket framing (RFC 6455), without I/O.
net.ws = {}

--- A Sec-WebSocket-Key for a client handshake.
function net.ws.generateKey(): string
	return nil :: any
end

--- The Sec-WebSocket-Accept value for a Sec-WebSocket-Key.
function net.ws.acceptKey(key: string): string
	return nil :: any
end

--- Frame bytes. `mask` is `true` (clients must mask) or a 4-byte key.
function net.ws.encode(
	opcode: Opcode,
	payload: string?,
	options: { mask: (boolean | string)?, fin: boolean?, rsv1: boolean? }?
): string
	return nil :: any
end

--- Returns `frame, bytesUsed`; `nil` if incomplete; or `nil, err`.
function net.ws.decode(data: string, options: { maxPayload: number? }?): (Frame?, number | string | nil)
	return nil :: any
end

--- The payload of a close frame.
function net.ws.closePayload(code: number?, reason: string?): string
	return nil :: any
end

--- `code?, reason`
function net.ws.parseClose(payload: string): (number?, string)
	return nil :: any, nil :: any
end

--- `data` XOR the 4-byte `key`.
function net.ws.mask(data: string, key: string): string
	return nil :: any
end

return net
