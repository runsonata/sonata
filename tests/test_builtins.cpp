// tests/test_builtins.cpp
#include "helper.hpp"
#include <doctest/doctest.h>
#include <memory>
#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/luau/vm.hpp>
#include <sonata/core/module.hpp>
#include <string_view>
#include <string>

TEST_SUITE("builtins") {

    TEST_CASE("Builtin path library is available through require") {
        Exec exec(R"(
        local path = require("@sonata/path")
        assert(path.version == 1)
        assert(path.posix.normalize("/a/./b//c/..") == "/a/b")
        assert(path.posix.relative("/a/b", "/a/c/d") == "../c/d")
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin fs library reads, writes, copies, queries, and removes "
              "files") {
        Exec exec(R"lua(
        local fs = require("@sonata/fs")
        assert(fs.version == 1 and fs.maxReadSize > 0)
        assert(fs.modes.file == 420 and fs.modes.privateDirectory == 448)
        local root = fs.makeTempDir("sonata-test-")
        root = fs.realPath(root)
        local source = root .. "/source"
        fs.makeDir(source .. "/nested", true)
        local file = source .. "/nested/data.bin"
        fs.writeFile(file, "A\0B")
        fs.appendFile(file, "C")
        assert(fs.readFile(file) == "A\0BC")
        assert(fs.exists(file) and fs.isFile(file) and not fs.isDir(file))
        assert(fs.stat(file).kind == "file" and fs.stat(file).size == 4)
        assert(fs.lstat(file).kind == "file" and fs.realPath(file) == file)
        local copied = root .. "/copied.bin"
        fs.copyFile(file, copied)
        assert(fs.readFile(copied) == "A\0BC")
        local moved = root .. "/moved.bin"
        fs.move(copied, moved)
        assert(not fs.exists(copied) and fs.readFile(moved) == "A\0BC")
        local names = fs.listDir(source, true)
        assert(#names == 2 and names[1] == "nested" and names[2] == "nested/data.bin")
        local entries = fs.readDir(source, true)
        assert(#entries == 2 and entries[2].name == "nested/data.bin" and entries[2].kind == "file")
        local destination = root .. "/destination"
        fs.copyDir(source, destination)
        assert(fs.readFile(destination .. "/nested/data.bin") == "A\0BC")
        fs.chmod(moved, fs.modes.readOnly)
        assert(fs.stat(moved).mode % 512 == fs.modes.readOnly)
        fs.chmod(moved, fs.modes.file)
        assert(fs.stat(moved).mode % 512 == fs.modes.file)
        local link = root .. "/link"
        if pcall(fs.symlink, moved, link) then
            assert(fs.isSymlink(link) and fs.readLink(link) == moved)
            assert(fs.stat(link).kind == "file" and fs.lstat(link).kind == "symlink")
        end
        local originalCwd = fs.cwd()
        fs.chdir(root)
        local cwdOk, cwdErr = pcall(function() assert(fs.cwd() == root) end)
        fs.chdir(originalCwd)
        assert(cwdOk, cwdErr)
        assert(fs.tempDir() ~= "" and not fs.isFile(root .. "/missing"))
        fs.removeFile(moved)
        fs.removeDir(root, true)
        assert(not fs.exists(root))
    )lua");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("fs diagnostic: cumulative steps") {
        const std::string header = R"lua(
        local fs = require("@sonata/fs")
        assert(fs.version == 1 and fs.maxReadSize > 0)
        assert(fs.modes.file == 420 and fs.modes.privateDirectory == 448)
        local root = fs.makeTempDir("sonata-test-")
        root = fs.realPath(root)
        local source = root .. "/source"
        fs.makeDir(source .. "/nested", true)
        local file = source .. "/nested/data.bin"
        fs.writeFile(file, "A\0B")
        fs.appendFile(file, "C")
    )lua";

        struct Step {
            const char *name;
            const char *body;
        };
        const Step steps[] = {
            {"read / exists / stat / lstat / realPath", R"lua(
            assert(fs.readFile(file) == "A\0BC")
            assert(fs.exists(file) and fs.isFile(file) and not fs.isDir(file))
            assert(fs.stat(file).kind == "file" and fs.stat(file).size == 4)
            assert(fs.lstat(file).kind == "file" and fs.realPath(file) == file)
    )lua"},
            {"copyFile / move", R"lua(
            local copied = root .. "/copied.bin"
            fs.copyFile(file, copied)
            assert(fs.readFile(copied) == "A\0BC")
            local moved = root .. "/moved.bin"
            fs.move(copied, moved)
            assert(not fs.exists(copied) and fs.readFile(moved) == "A\0BC")
    )lua"},
            {"listDir / readDir / copyDir", R"lua(
            local names = fs.listDir(source, true)
            assert(#names == 2 and names[1] == "nested" and names[2] == "nested/data.bin")
            local entries = fs.readDir(source, true)
            assert(#entries == 2 and entries[2].name == "nested/data.bin" and entries[2].kind == "file")
            local destination = root .. "/destination"
            fs.copyDir(source, destination)
            assert(fs.readFile(destination .. "/nested/data.bin") == "A\0BC")
    )lua"},
            {"chmod (readOnly / file)", R"lua(
            fs.chmod(moved, fs.modes.readOnly)
            assert(fs.stat(moved).mode % 512 == fs.modes.readOnly)
            fs.chmod(moved, fs.modes.file)
            assert(fs.stat(moved).mode % 512 == fs.modes.file)
    )lua"},
            {"symlink", R"lua(
            local link = root .. "/link"
            if pcall(fs.symlink, moved, link) then
                assert(fs.isSymlink(link) and fs.readLink(link) == moved)
                assert(fs.stat(link).kind == "file" and fs.lstat(link).kind == "symlink")
            end
    )lua"},
            {"cwd / chdir / tempDir", R"lua(
            local originalCwd = fs.cwd()
            fs.chdir(root)
            local cwdOk, cwdErr = pcall(function() assert(fs.cwd() == root) end)
            fs.chdir(originalCwd)
            assert(cwdOk, cwdErr)
            assert(fs.tempDir() ~= "" and not fs.isFile(root .. "/missing"))
    )lua"},
            {"removeFile / removeDir", R"lua(
            fs.removeFile(moved)
            fs.removeDir(root, true)
            assert(not fs.exists(root))
    )lua"},
        };

        std::string script = header;
        for (const Step &step : steps) {
            script += step.body;
            Exec exec(script.c_str());
            CHECK_MESSAGE(exec.getStatus() == 0,
                          "FAILED after step: " << step.name);
        }

        const std::string privateScript = header + steps[0].body +
                                          steps[1].body + steps[2].body +
                                          R"lua(
            fs.chmod(moved, fs.modes.private)
            assert(fs.stat(moved).mode % 512 == fs.modes.private)
    )lua";
        Exec original(privateScript.c_str());
#ifdef _WIN32
        CHECK_MESSAGE(original.getStatus() != 0,
                      "0600 round-trip unexpectedly WORKS on Windows; the old "
                      "assertion is not the problem");
#else
        CHECK_MESSAGE(original.getStatus() == 0,
                      "0600 round-trip failed on a POSIX platform");
#endif
    }

    TEST_CASE("Builtin path library covers both lexical path styles") {
        Exec exec(R"(
        local path = require("@sonata/path")
        local p = path.posix
        assert(path.version == 1 and p.version == 1 and path.win32.version == 1)
        assert(p.sep == "/" and p.delimiter == ":" and p.style == "posix")
        assert(p.normalize("/a/./b//c/..") == "/a/b")
        assert(p.join("a", "b/", "../c") == "a/c" and p.join() == ".")
        assert(p.relative("/a/b", "/a/c/d") == "../c/d")
        assert(p.isInside("/a/b/c", "/a/b") and p.isInside("/a/b", "/a/b"))
        assert(p.dirname("/a/b/c.txt") == "/a/b")
        assert(p.basename("/a/b/c.txt") == "c.txt")
        assert(p.basename("/a/b/c.txt", ".txt") == "c")
        assert(p.extname("/a/b/c.tar.gz") == ".gz" and p.stem("/a/b/c.tar.gz") == "c.tar")
        assert(p.withExtension("a/b.txt", "md") == "a/b.md")
        assert(p.withExtension("a/b.txt", "") == "a/b")
        assert(p.isAbsolute("/a") and not p.isAbsolute("a"))
        local parts = p.components("/a/b/../c")
        assert(parts[1] == "/" and parts[2] == "a" and parts[4] == "..")
        local parsed = p.parse("/a/b.txt")
        assert(parsed.root == "/" and parsed.dir == "/a" and parsed.base == "b.txt")
        assert(parsed.ext == ".txt" and parsed.name == "b")
        assert(p.format({ dir = "/a", name = "b", ext = "txt" }) == "/a/b.txt")
        assert(path.win32.normalize("C:\\a\\..\\b") == "C:\\b")
        assert(path.win32.toPosix("a\\b") == "a/b")
        assert(path.win32.toNative("a/b") == "a\\b")
        local list = p.splitList(":/bin::/usr/bin:")
        assert(#list == 2 and list[1] == "/bin" and list[2] == "/usr/bin")
        assert(p.joinList({ "/bin", "/usr/bin" }) == "/bin:/usr/bin")
        assert(type(p.resolve("src", "../lib")) == "string")
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin json library parses, encodes, and modifies values") {
        Exec exec(R"(
        local json = require("@sonata/json")
        assert(json.version == 1 and json.null ~= nil)
        local value = json.decode('{"a":[1,2,{"b":null}],"z":true}')
        assert(value.a[3].b == json.null and value.z == true)
        assert(json.encode({ x = 1, y = { true, "s" } }) == '{"x":1,"y":[true,"s"]}')
        assert(string.find(json.encode(value, { indent = 2 }), "\n  \"a\"", 1, true))
        assert(json.get(value, "a[3].b") == json.null)
        assert(json.get(value, "a[8].missing", "fallback") == "fallback")
        json.set(value, "a[1]", 5)
        json.set(value, "new.deep", "yes")
        assert(value.a[1] == 5 and value.new.deep == "yes")
        assert(json.delete(value, "a[2]") == 2 and value.a[2]["b"] == json.null)
        local clone = json.clone(value)
        json.set(clone, "new.deep", "changed")
        assert(value.new.deep == "yes" and clone.new.deep == "changed")
        json.merge(value, { new = json.null, added = 3 })
        assert(value.new == nil and value.added == 3)
        assert(json.encode(json.array()) == "[]" and json.encode(json.object()) == "{}")
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin sys library reports machine facts and clocks") {
        Exec exec(R"(
        local sys = require("@sonata/sys")
        assert(sys.eol == "\n" or sys.eol == "\r\n")
        assert(sys.pathSeparator == "/" or sys.pathSeparator == "\\")
        assert(sys.pathDelimiter == ":" or sys.pathDelimiter == ";")
        assert(type(sys.devNull) == "string" and #sys.devNull > 0)
        assert(type(sys.cpuCount()) == "number" and sys.cpuCount() >= 1)
        assert(type(sys.pid()) == "number" and sys.pid() > 0)
        assert(type(sys.time()) == "number" and sys.time() > 0)
        local before = sys.clock()
        sys.sleep(0)
        assert(sys.clock() >= before)
        assert(type(sys.osName()) == "string" and type(sys.arch()) == "string")
        assert(type(sys.info()) == "table" and sys.info().pid == sys.pid())
        assert(type(sys.hostname()) == "string" or sys.hostname() == nil)
        assert(type(sys.username()) == "string" or sys.username() == nil)
        assert(type(sys.homeDir()) == "string" or sys.homeDir() == nil)
        assert(type(sys.tempDir()) == "string" or sys.tempDir() == nil)
        assert(type(sys.osVersion()) == "string" or sys.osVersion() == nil)
        assert(type(sys.arch()) == "string" and sys.pageSize() >= 0)
        assert(sys.totalMemory() == nil or sys.totalMemory() > 0)
        assert(sys.freeMemory() == nil or sys.freeMemory() >= 0)
        assert(sys.uptime() == nil or sys.uptime() >= 0)
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin task library starts, queues, and cancels work") {
        Exec exec(R"(
        local task = require("@sonata/task")
        local called = 0
        local thread = task.spawn(function(value) called = value end, 7)
        assert(type(thread) == "thread" and called == 7)
        local deferred = task.defer(function() called = 8 end)
        local delayed = task.delay(0, function() called = 9 end)
        assert(type(deferred) == "thread" and type(delayed) == "thread")
        task.cancel(deferred)
        task.cancel(delayed)
        assert(called == 7)
        local ok = pcall(task.wait, 0)
        assert(not ok)
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin process library exposes process state and environment") {
        Exec exec(R"(
        local process = require("@sonata/process")
        assert(type(process.os) == "string" and type(process.arch) == "string")
        assert(process.endianness == "little" or process.endianness == "big")
        assert(type(process.pid) == "number" and process.pid > 0)
        assert(type(process.args) == "table" and type(process.cwd()) == "string")
        local originalCwd = process.cwd()
        process.chdir(originalCwd)
        assert(process.cwd() == originalCwd)
        local previous = process.env.SONATA_BUILTIN_TEST_VALUE
        process.env.SONATA_BUILTIN_TEST_VALUE = "present"
        assert(process.env.SONATA_BUILTIN_TEST_VALUE == "present")
        local found = false
        for name, value in process.env do
            if name == "SONATA_BUILTIN_TEST_VALUE" and value == "present" then found = true end
        end
        assert(found)
        process.env.SONATA_BUILTIN_TEST_VALUE = previous
        assert(type(process.exec) == "function" and type(process.exit) == "function")
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin stdio library writes and describes streams") {
        Exec exec(R"(
        local stdio = require("@sonata/stdio")
        assert(stdio.stdin ~= nil and stdio.stdout ~= nil and stdio.stderr ~= nil)
        assert(type(stdio.stdin:isTerminal()) == "boolean")
        assert(type(stdio.stdout:isTerminal()) == "boolean")
        local columns, rows = stdio.stdout:size()
        assert((columns == nil) == (rows == nil))
        assert(tostring(stdio.stdout) == "stdio.stdout")
        assert(stdio.stdout:write("stdio-"))
        assert(stdio.stdout:writeLine("ok"))
        assert(stdio.stdout:flush())
        assert(not pcall(function() stdio.stdin:write("bad") end))
        assert(not pcall(function() stdio.stdout:read(1) end))
        assert(type(stdio.prompt) == "function")
    )");
        REQUIRE(exec.getStatus() == 0);
        CHECK(exec.getOutput() == "stdio-ok\n");
    }

    TEST_CASE(
        "Builtin net library parses URLs and exchanges loopback traffic") {
        Exec exec(R"(
        local net = require("@sonata/net")
        assert(net.version == 1)
        local url = net.url.parse("https://u:p@Host:8443/a/b?x=1#top")
        assert(url.scheme == "https" and url.username == "u" and url.password == "p")
        assert(url.host == "host" and url.port == 8443 and url.path == "/a/b")
        assert(url.query == "x=1" and url.fragment == "top")
        assert(net.url.encode("a b/c", "/") == "a%20b/c")
        assert(net.url.decode("a%20b+c", true) == "a b c")
        local query, pairs = net.url.parseQuery("a=1&b=2&a=3")
        assert(query.a == "1" and query.b == "2" and #pairs == 3)
        assert(net.url.buildQuery({ { "a", "1" }, { "a", "2" } }) == "a=1&a=2")
        assert(net.url.build({ scheme = "https", host = "a.com", path = "/s", query = { q = "a b" } }) == "https://a.com/s?q=a%20b")

        local server = assert(net.listen("127.0.0.1", 0))
        server:setTimeout(2)
        local host, port = server:localAddress()
        assert(type(host) == "string" and port > 0)
        local client = assert(net.connect(host, port, 2))
        client:setTimeout(2)
        client:setOption("nodelay", true)
        local peer = assert(server:accept())
        peer:setTimeout(2)
        assert(client:send("hello! ") == 7)
        assert(peer:recvUntil("!") == "hello!")
        assert(peer:recv(1) == " ")
        local peerHost, peerPort = peer:peerAddress()
        assert(type(peerHost) == "string" and peerPort > 0)
        assert(client:shutdown("write"))
        assert(peer:recv() == "")
        assert(type(client:tlsInfo()) == "nil")
        client:close()
        peer:close()
        server:close()

        local receiver = assert(net.udp("127.0.0.1", 0))
        receiver:setTimeout(2)
        local receiverHost, receiverPort = receiver:localAddress()
        local sender = assert(net.udp())
        assert(sender:sendTo("datagram", receiverHost, receiverPort) == 8)
        local data, senderHost, senderPort = receiver:recvFrom()
        assert(data == "datagram" and type(senderHost) == "string" and senderPort > 0)
        assert(type(net.resolve("127.0.0.1", "ipv4")) == "table")
        sender:close()
        receiver:close()
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin crypto library hashes, encodes, encrypts, and signs") {
        Exec exec(R"(
        local crypto = require("@sonata/crypto")
        assert(crypto.version == 1 and type(crypto.hardwareAes) == "boolean")
        assert(#crypto.hashes > 0 and #crypto.ciphers > 0)
        assert(crypto.hash("sha256", "abc", "hex") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        assert(#crypto.hmac("sha256", "key", "data") == 32)
        assert(#crypto.pbkdf2("sha256", "password", "salt", 2, 16) == 16)
        local derived, iterations = crypto.pbkdf2Timed("sha256", "password", "salt", 1, 16)
        assert(#derived == 16 and iterations >= 1)
        assert(#crypto.hkdf("sha256", "input", "salt", "info", 16) == 16)
        assert(#crypto.scrypt("password", "salt", 2, 1, 1, 16) == 16)

        local encrypted = assert(crypto.encrypt("aes-256-gcm", string.rep("k", 32), "secret"))
        assert(crypto.decrypt("aes-256-gcm", string.rep("k", 32), encrypted) == "secret")
        assert(#crypto.randomBytes(16) == 16)
        assert(crypto.randomInt(5, 5) == 5)
        assert(#crypto.uuid() == 36 and #crypto.uuid(5, "dns", "www.example.com") == 36)
        assert(#crypto.ulid() == 26)
        assert(crypto.totp("12345678901234567890", { digits = 8, time = 59 }) == "94287082")
        local password = crypto.hashPassword("test-password", 1)
        assert(crypto.verifyPassword("test-password", password))
        assert(not crypto.verifyPassword("wrong", password))
        assert(crypto.encode("hex", "hello") == "68656c6c6f")
        assert(crypto.decode("hex", "68656c6c6f") == "hello")
        assert(crypto.decode("hex", "zz") == nil)
        assert(crypto.equals("same", "same") and not crypto.equals("same", "else"))

        local publicKey, privateKey = crypto.keypair("ed25519")
        local signature = crypto.sign(privateKey, "message")
        assert(crypto.verify(publicKey, "message", signature))
        assert(not crypto.verify(publicKey, "other", signature))
        local publicA, privateA = crypto.keypair("x25519")
        local publicB, privateB = crypto.keypair("x25519")
        local sharedA = crypto.sharedSecret(privateA, publicB)
        local sharedB = crypto.sharedSecret(privateB, publicA)
        assert(crypto.equals(sharedA, sharedB))
        local sealed = crypto.seal(publicA, "sealed message")
        assert(crypto.open(privateA, sealed) == "sealed message")
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin datetime library converts, parses, and performs "
              "calendar arithmetic") {
        Exec exec(R"(
        local datetime = require("@sonata/datetime")
        assert(datetime.version == 1)
        local t, nsec = datetime.fromFields({ year = 2024, month = 2, day = 29, hour = 12, min = 34, sec = 56, nsec = 123456789 }, "UTC")
        local fields = datetime.toFields(t, "UTC", nsec)
        assert(fields.year == 2024 and fields.month == 2 and fields.day == 29)
        assert(fields.hour == 12 and fields.min == 34 and fields.sec == 56 and fields.nsec == 123456789)
        assert(fields.wday >= 1 and fields.wday <= 7 and fields.yday == 60)
        local text = datetime.format(t, "%Y-%m-%d %H:%M:%S.%N", "UTC", nsec)
        assert(text == "2024-02-29 12:34:56.123456789")
        local parsed, parsedNsec = datetime.parse(text, "%Y-%m-%d %H:%M:%S.%N", "UTC")
        assert(parsed == t and parsedNsec == nsec)
        local nextDay, nextNsec = datetime.add(t, { days = 1 }, "UTC", nsec)
        local nextFields = datetime.toFields(nextDay, "UTC", nextNsec)
        assert(nextFields.month == 3 and nextFields.day == 1)
        local start, startNsec = datetime.startOf(t, "day", "UTC", nsec)
        local startFields = datetime.toFields(start, "UTC", startNsec)
        assert(startFields.hour == 0 and startFields.nsec == 0)
        assert(datetime.convert(1, "unix", "unixms") == 1000)
        assert(datetime.isLeapYear(2000) and not datetime.isLeapYear(1900))
        assert(datetime.daysInMonth(2024, 2) == 29 and datetime.daysInMonth(2023, 2) == 28)
        local now, nowNsec = datetime.now()
        assert(type(now) == "number" and nowNsec >= 0 and nowNsec < 1000000000)
        assert(type(datetime.monotonic()) == "number")
        local invalid = datetime.parse("not a date")
        assert(invalid == nil)
    )");
        REQUIRE(exec.getStatus() == 0);
    }

    TEST_CASE("Builtin compress library is available through require") {
        Exec exec(R"(
        local compress = require("@sonata/compress")
        assert(compress.version == 2)
        local blob = compress.compress("hello hello hello hello")
        assert(compress.detect(blob) == "gzip")
        assert(compress.decompress(blob) == "hello hello hello hello")
        assert(compress.decompress(compress.compress("abc", "zlib"), "zlib") == "abc")
        assert(compress.crc32("abc") == 891568578)
    )");
    REQUIRE(exec.getStatus() == 0);
}
}