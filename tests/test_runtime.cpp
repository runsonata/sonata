// tests/test_runtime.cpp: the scheduler as programs see it, and the newer parts of
// fs and net.
#include "helper.hpp"
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/module.hpp>
#include <sonata/core/runtime.hpp>

namespace luau = sonata::luau;

namespace {

struct ProgramResult {
    std::string error;
    std::string output;
};

// Runs "source" as a program's entry script, the way sn run does.
ProgramResult runProgram(std::string_view source, const std::vector<std::string>& args = {}) {
    auto files = std::make_unique<luau::MemorySource>();
    files->add("/main.luau", luau::Compiler().compile(source));
    luau::ModuleLoader loader(std::move(files));

    sonata::runtime::ProgramInfo info;
    info.name = "test";
    info.entry = "/main.luau";

    ProgramResult result;
    StdoutCapture capture;
    try {
        sonata::runtime::runProgram(loader, info, args, nullptr);
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    result.output = capture.finish();
    return result;
}

// A self-signed certificate for "localhost" / 127.0.0.1, valid until 2126. Test use only.
constexpr const char* kTestCert = R"(-----BEGIN CERTIFICATE-----
MIIBmzCCAUGgAwIBAgIUfNXb/sP0tLIMVnvsQRAMdG2rzc4wCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAxMTE5NDA0MVoYDzIxMjYwOTE3
MTk0MDQxWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAASnL/ElE6tyor4UWMWuW+VUb83scNxEarrIHGMoJPw6YJKfokWcFvaT
DPSBO/UeMAAtzkOe1+6nfUwT7dyV8wi3o28wbTAdBgNVHQ4EFgQU4GKGg1Ostgf8
58Jig6YGN1I0xvAwHwYDVR0jBBgwFoAU4GKGg1Ostgf858Jig6YGN1I0xvAwDwYD
VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SHBH8AAAEwCgYIKoZI
zj0EAwIDSAAwRQIhAKDJI2Z2EkMoYPy0gpxn+QopoKrw69lwrePIM+4Kq7g9AiAI
oW/It+5DcVnDuQUvqm3/8iWKcI6IxBTmZW15OuNsGg==
-----END CERTIFICATE-----
)";

constexpr const char* kTestKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgaJnW92bLlv4YrUOl
Uol4i9cPV6sNhNCLbspUWU9f8quhRANCAASnL/ElE6tyor4UWMWuW+VUb83scNxE
arrIHGMoJPw6YJKfokWcFvaTDPSBO/UeMAAtzkOe1+6nfUwT7dyV8wi3
-----END PRIVATE KEY-----
)";

} // namespace

TEST_SUITE("runtime") {

    TEST_CASE("The entry script runs as a task and tasks outlive it") {
        const ProgramResult result = runProgram(R"(
        local task = require("@sonata/task")
        task.delay(0.02, function() print("later") end)
        task.spawn(function() task.wait(0.01) print("background") end)
        local elapsed = task.wait(0.005)  -- top-level waits suspend the entry like any task
        print("main", elapsed >= 0.004, task.running() ~= nil)
    )");
        REQUIRE_MESSAGE(result.error.empty(), result.error);
        CHECK(result.output == "main\ttrue\ttrue\nbackground\nlater\n");
    }

    TEST_CASE("An error in the entry stops the program, even after it waited") {
        const ProgramResult result = runProgram(R"(
        local task = require("@sonata/task")
        task.delay(1, function() print("never") end)
        task.wait(0.01)
        error("entry failed")
    )");
        CHECK(result.error.find("entry failed") != std::string::npos);
        CHECK(result.error.find("stacktrace") != std::string::npos);
        CHECK(result.output.find("never") == std::string::npos);
    }

    TEST_CASE("Errors in other tasks do not stop the program") {
        const ProgramResult result = runProgram(R"(
        local task = require("@sonata/task")
        local reported
        task.onError(function(message) reported = message end)
        task.spawn(function() error("task failed") end)
        task.wait(0.01)
        print(string.find(reported, "task failed") ~= nil)
    )");
        REQUIRE_MESSAGE(result.error.empty(), result.error);
        CHECK(result.output == "true\n");
    }

    TEST_CASE("A bare coroutine.yield at the top level is still an error") {
        const ProgramResult result = runProgram("coroutine.yield()");
        CHECK(result.error.find("yielded unexpectedly") != std::string::npos);
    }

    TEST_CASE("process.args holds the program's arguments") {
        const ProgramResult result = runProgram(R"(
        local process = require("@sonata/process")
        print(#process.args, process.args[1], process.args[2], args[2])
    )", {"first", "second"});
        REQUIRE_MESSAGE(result.error.empty(), result.error);
        CHECK(result.output == "2\tfirst\tsecond\tsecond\n");
    }

    TEST_CASE("Sockets let other tasks run while they wait") {
        const ProgramResult result = runProgram(R"(
        local net = require("@sonata/net")
        local task = require("@sonata/task")
        local server = assert(net.listen("127.0.0.1", 0))
        local _, port = server:localAddress()
        task.spawn(function()
            while true do
                local client = server:accept()
                if not client then return end
                task.spawn(function()
                    local line = client:recvLine()
                    task.wait(0.01)
                    client:send("echo " .. line .. "\n")
                    client:close()
                end)
            end
        end)
        local replies = task.all({
            function() local c = assert(net.connect("127.0.0.1", port, 2)) c:send("a\n") return c:recvLine() end,
            function() local c = assert(net.connect("127.0.0.1", port, 2)) c:send("b\n") return c:recvLine() end,
        })
        print(replies[1], replies[2])
        server:close()
    )");
        REQUIRE_MESSAGE(result.error.empty(), result.error);
        CHECK(result.output == "echo a\techo b\n");
    }
}

TEST_SUITE("builtins") {

    TEST_CASE("Builtin fs library walks, globs, watches, and locks") {
        Exec exec(R"(
        local fs = require("@sonata/fs")
        local task = require("@sonata/task")
        local root = fs.realPath(fs.makeTempDir("sonata-test-"))
        for _, p in { "a.txt", "b.luau", "src/x.luau", "src/deep/y.luau", "src/deep/z.txt", ".hidden/h.luau" } do
            local dir = (root .. "/" .. p):match("^(.*)/[^/]*$")
            fs.makeDir(dir, true)
            fs.writeFile(root .. "/" .. p, p)
        end

        assert(table.concat(fs.glob("**/*.luau", { cwd = root }), " ") == "b.luau src/deep/y.luau src/x.luau")
        assert(table.concat(fs.glob("src/{*,deep/*}.{luau,txt}", { cwd = root }), " ") == "src/deep/y.luau src/deep/z.txt src/x.luau")
        assert(#fs.glob("**/*.luau", { cwd = root, dot = true }) == 4)
        assert(table.concat(fs.glob("*", { cwd = root, kind = "directory" }), " ") == "src")
        assert(fs.glob(root .. "/src/*.luau")[1] == root .. "/src/x.luau")
        assert(table.concat(fs.glob("[ab].*", { cwd = root }), " ") == "a.txt b.luau")
        assert(fs.match("src/**/*.luau", "src/deep/y.luau") and not fs.match("*.txt", "src/a.txt"))
        assert(fs.match("**/*.txt", "./src/deep/z.txt") and not fs.match("*.luau", ".x.luau"))
        assert(fs.match("*.luau", ".x.luau", { dot = true }) and fs.match("[!a]?", "bc"))

        local seen = {}
        for entry in fs.walk(root, { filter = function(e) return e.name ~= ".hidden" end }) do
            table.insert(seen, entry.path .. ":" .. entry.kind:sub(1, 1) .. entry.depth)
        end
        assert(table.concat(seen, " ") == "a.txt:f1 b.luau:f1 src:d1 src/deep:d2 src/deep/y.luau:f3 src/deep/z.txt:f3 src/x.luau:f2")
        seen = {}
        for entry in fs.walk(root, { maxDepth = 1, dirs = false }) do table.insert(seen, entry.path) end
        assert(table.concat(seen, " ") == "a.txt b.luau")

        local watcher = fs.watch(root, { interval = 0.02 })
        task.delay(0.03, function() fs.writeFile(root .. "/new.txt", "x") end)
        local event = watcher:next(2)
        assert(event and event.kind == "create" and event.path == root .. "/new.txt" and not event.isDir)
        fs.writeFile(root .. "/a.txt", "changed, and longer")
        fs.removeFile(root .. "/b.luau")
        local kinds = {}
        for _, e in watcher:poll() do kinds[e.kind] = e.path end
        assert(kinds.modify == root .. "/a.txt" and kinds.remove == root .. "/b.luau")
        assert(watcher:next(0.05) == nil)
        watcher:close()
        assert(watcher:next() == nil)

        local lockPath = root .. "/lock"
        local lock = assert(fs.lock(lockPath))
        assert(lock:isLocked() and fs.tryLock(lockPath) == nil)
        assert(fs.lock(lockPath, { timeout = 0.02 }) == nil)
        task.delay(0.02, function() lock:unlock() end)
        local second = fs.lock(lockPath, { timeout = 2 })
        assert(second and not lock:isLocked())
        second:unlock()
        local s1 = fs.lock(lockPath, { shared = true })
        local s2 = fs.tryLock(lockPath, { shared = true })
        assert(s1 and s2 and fs.tryLock(lockPath) == nil)
        assert(not pcall(fs.lock, root .. "/missing/dir/lock"))
        s1:unlock()
        s2:unlock()
        fs.removeDir(root, true)
    )");
        REQUIRE_MESSAGE(exec.getStatus() == 0, exec.getError());
    }

    TEST_CASE("Builtin net library speaks HTTP/1.1 and WebSocket framing") {
        Exec exec(R"(
        local net = require("@sonata/net")
        local http, ws = net.http, net.ws

        local raw = "\r\nPOST /api/x?y=1 HTTP/1.1\r\nHost: a.com\r\nX-A: 1\r\nx-a: 2\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nContent-Length: 5\r\n\r\nhelloNEXT"
        assert(http.parseRequest(raw:sub(1, 30)) == nil)
        local req, used = http.parseRequest(raw)
        assert(req.method == "POST" and req.path == "/api/x" and req.query == "y=1" and req.version == "1.1")
        assert(req.headers["x-a"] == "1, 2" and req.headers["set-cookie"] == "a=1\nb=2" and #req.headerList == 6)
        assert(raw:sub(used + 1) == "helloNEXT")
        local kind, length = http.bodyLength(req)
        assert(kind == "length" and length == 5 and http.keepAlive(req))
        assert(http.parseRequest("GET / HTTP/1.1\r\n folded: x\r\n\r\n") == nil)
        assert(select(2, http.parseRequest("GET / HTTP/1.1\r\nBad Header: x\r\n\r\n")) == "invalid header line")

        local res = http.parseResponse("HTTP/1.0 404 Not Found\r\nTransfer-Encoding: gzip, chunked\r\nConnection: keep-alive\r\n\r\n")
        assert(res.status == 404 and res.reason == "Not Found" and http.bodyLength(res) == "chunked")
        assert(http.keepAlive(res) and http.bodyLength(res, { requestMethod = "HEAD" }) == "none")
        assert(http.bodyLength({ status = 200, headers = {} }) == "close")
        assert(http.bodyLength({ headers = { ["Content-Length"] = "5, 6" } }) == nil)

        assert(http.formatRequest({ method = "GET", target = "/", headers = { Host = "a.com", Accept = { "a", "b" } } }) ==
            "GET / HTTP/1.1\r\nAccept: a\r\nAccept: b\r\nHost: a.com\r\n\r\n")
        assert(http.formatResponse({ status = 201, headers = { { "X", "1" } } }) == "HTTP/1.1 201 Created\r\nX: 1\r\n\r\n")
        assert(not pcall(http.formatResponse, { status = 200, headers = { X = "a\r\nb" } }))
        assert(http.statusText(404) == "Not Found" and http.statusText(999) == nil)

        local decoder = http.chunkedDecoder()
        local encoded = http.encodeChunk("hello") .. http.encodeChunk(" world") .. http.lastChunk({ Expires = "never" }) .. "NEXT"
        local body, done = "", false
        for i = 1, #encoded, 3 do
            local piece, finished = assert(decoder:push(encoded:sub(i, i + 2)))
            body ..= piece
            done = finished
        end
        assert(body == "hello world" and done and decoder:rest() == "NEXT" and decoder:trailers().expires == "never")
        assert(http.chunkedDecoder():push("zz\r\n") == nil)

        local t = 784111777
        assert(http.formatDate(t) == "Sun, 06 Nov 1994 08:49:37 GMT")
        assert(http.parseDate("Sun, 06 Nov 1994 08:49:37 GMT") == t)
        assert(http.parseDate("Sunday, 06-Nov-94 08:49:37 GMT") == t and http.parseDate("Sun Nov  6 08:49:37 1994") == t)

        local value, params = http.parseHeaderValue('attachment; filename="a b.txt"; Size=10')
        assert(value == "attachment" and params.filename == "a b.txt" and params.size == "10")
        assert(table.concat(http.parseList('a, "b,c" , d'), "|") == 'a|"b,c"|d')
        local q = http.parseQuality("gzip;q=0.8, br, *;q=0.1")
        assert(q[1].value == "br" and q[2].value == "gzip" and q[2].q == 0.8 and q[3].value == "*")
        local user, pass = http.parseBasicAuth(http.basicAuth("user", "p:w"))
        assert(user == "user" and pass == "p:w")

        local cookies = http.parseCookies('a=1; b="2"; a=3')
        assert(cookies.a == "1" and cookies.b == "2")
        local cookie = http.parseSetCookie("id=abc; Path=/; Domain=.Example.com; Max-Age=60; Secure; HttpOnly; SameSite=lax")
        assert(cookie.name == "id" and cookie.value == "abc" and cookie.path == "/" and cookie.domain == "example.com")
        assert(cookie.maxAge == 60 and cookie.secure and cookie.httpOnly and cookie.sameSite == "Lax")
        assert(http.formatSetCookie({ name = "id", value = "abc", path = "/", secure = true, sameSite = "strict" }) ==
            "id=abc; Path=/; Secure; SameSite=Strict")
        assert(http.formatCookies({ b = "2", a = "1" }) == "a=1; b=2")
        assert(not pcall(http.formatSetCookie, { name = "a b", value = "x" }))

        local form, contentType = http.encodeMultipart({
            { name = "field", value = "v1" },
            { name = "file", filename = "a.txt", value = "data\r\nmore", contentType = "text/plain" },
        })
        local parts = assert(http.decodeMultipart(form, contentType))
        assert(#parts == 2 and parts[1].name == "field" and parts[1].value == "v1")
        assert(parts[2].filename == "a.txt" and parts[2].contentType == "text/plain" and parts[2].value == "data\r\nmore")

        assert(net.mime.lookup("x/photo.PNG") == "image/png" and net.mime.lookup("json") == "application/json")
        assert(net.mime.extension("text/html; charset=utf-8") == "html" and net.mime.lookup("nope") == nil)
        assert(net.mime.parse("Text/HTML; Charset=UTF-8").essence == "text/html" and net.mime.isText("application/ld+json"))

        assert(ws.acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
        local frame = ws.encode("text", "Hello", { mask = true })
        local decoded, consumed = ws.decode(frame)
        assert(decoded.opcode == "text" and decoded.payload == "Hello" and decoded.masked and consumed == #frame)
        assert(ws.decode(frame:sub(1, 3)) == nil)
        assert(#ws.decode(ws.encode("binary", string.rep("x", 70000))).payload == 70000)
        local code, reason = ws.parseClose(ws.closePayload(1000, "bye"))
        assert(code == 1000 and reason == "bye")
        assert(not pcall(ws.encode, "ping", string.rep("x", 200)))
        assert(select(2, ws.decode(string.char(0x89, 126, 0, 200))) == "invalid control frame")
    )");
        REQUIRE_MESSAGE(exec.getStatus() == 0, exec.getError());
    }

    TEST_CASE("Builtin net library handles URLs, names, and addresses") {
        Exec exec(R"(
        local net = require("@sonata/net")
        local url, ip = net.url, net.ip

        local base = "http://a/b/c/d;p?q"
        assert(url.resolve(base, "../g") == "http://a/b/g" and url.resolve(base, "//g") == "http://g")
        assert(url.resolve(base, "?y") == "http://a/b/c/d;p?y" and url.resolve(base, "../../../g") == "http://a/g")
        assert(url.resolve("https://x.com/a/", "b?c#d") == "https://x.com/a/b?c#d")
        assert(url.normalize("HTTP://Example.COM:80/a/./b/../c/%7euser?q=%2f") == "http://example.com/a/c/~user?q=%2F")
        assert(url.origin("https://Ex.com:443/x") == "https://ex.com" and url.origin("http://a.com:8080/") == "http://a.com:8080")
        assert(url.defaultPort("wss") == 443 and url.defaultPort("nope") == nil)
        assert(url.buildQuery({ b = "2", a = { "1", "x" } }) == "a=1&a=x&b=2")

        assert(net.idna.toAscii("Bücher.de") == "xn--bcher-kva.de" and net.idna.toUnicode("xn--bcher-kva.de") == "bücher.de")
        assert(net.idna.toAscii("例え.テスト") == "xn--r8jz45g.xn--zckzah")

        assert(ip.isValid("1.2.3.4") and ip.version("::1") == 6 and ip.version("x") == nil)
        assert(ip.normalize("2001:0db8:0000::0001") == "2001:db8::1")
        assert(ip.kind("10.1.2.3") == "private" and ip.kind("8.8.8.8") == "public" and ip.kind("::ffff:127.0.0.1") == "loopback")
        assert(ip.kind("fe80::1%eth0") == "linkLocal" and ip.isPublic("1.1.1.1") and not ip.isPublic("192.168.0.1"))
        assert(ip.inRange("192.168.1.7", "192.168.0.0/16") and ip.inRange("10.0.0.1", { "192.168.0.0/16", "10.0.0.0/8" }))
        assert(not ip.inRange("::1", "10.0.0.0/8"))
        local cidr = ip.parseCidr("192.168.1.77/26")
        assert(cidr.network == "192.168.1.64/26" and cidr.first == "192.168.1.64" and cidr.last == "192.168.1.127")
        assert(cidr.size == 64 and cidr.mask == "255.255.255.192")
        assert(ip.fromBytes(ip.parse("127.0.0.1").bytes) == "127.0.0.1")

        local loopback = false
        for _, interface in net.interfaces() do
            loopback = loopback or (interface.internal and interface.family == "ipv4" and interface.address == "127.0.0.1")
        end
        assert(loopback)
    )");
        REQUIRE_MESSAGE(exec.getStatus() == 0, exec.getError());
    }

    TEST_CASE("Builtin net library speaks TLS") {
        const std::string script = std::string(R"(
        local net = require("@sonata/net")
        local task = require("@sonata/task")
        local cert, key = [==[)") + kTestCert + "]==], [==[" + kTestKey + R"(]==]
        assert(net.tls.available)
        local context = assert(net.tls.context({ server = true, certPem = cert, keyPem = key, alpn = { "h2", "http/1.1" } }))
        local server = assert(net.listen("127.0.0.1", 0))
        local _, port = server:localAddress()
        task.spawn(function()
            for _ = 1, 3 do
                local client = server:accept()
                task.spawn(function()
                    if client:startTls(context) then
                        local line = client:recvLine()
                        if line then client:send("secure " .. line .. "\n") end
                    end
                    client:close()
                end)
            end
        end)

        local c = assert(net.connect("127.0.0.1", port, { timeout = 5, tls = { caPem = cert, host = "localhost", alpn = { "http/1.1" } } }))
        c:send("hello\n")
        assert(c:recvLine() == "secure hello")
        local info = c:tlsInfo()
        assert(info.alpn == "http/1.1" and info.version:find("TLS"))
        local peer = c:peerCertificate()
        assert(peer.subject == "CN=localhost" and #peer.fingerprint == 64 and table.concat(peer.altNames, ",") == "localhost,127.0.0.1")
        c:close()

        local refused, err = net.connect("127.0.0.1", port, { timeout = 5, tls = { host = "localhost" } })
        assert(refused == nil and err:find("certificate verify", 1, true) or err:find("verification", 1, true))

        local plain = assert(net.connect("127.0.0.1", port, 5))
        assert(plain:startTls({ verify = false }))
        plain:send("upgraded\n")
        assert(plain:recvLine() == "secure upgraded")
        plain:close()
        server:close()
    )";
        Exec exec(script);
        REQUIRE_MESSAGE(exec.getStatus() == 0, exec.getError());
    }
}
