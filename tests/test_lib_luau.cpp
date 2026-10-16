// tests/test_lib_luau.cpp: the @sonata/luau built-in library.
#include "helper.hpp"
#include <doctest/doctest.h>
#include <memory>
#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/luau/vm.hpp>
#include <sonata/core/module.hpp>
#include <string>
#include <string_view>
#include "lua.h"

namespace {

// Every script starts with the library in scope.
std::string withLuau(std::string_view body) {
    return "local luau = require(\"@sonata/luau\")\n" + std::string(body);
}

#define RUN_LUAU(body)                                                                                                 \
    do {                                                                                                               \
        Exec exec(withLuau(body));                                                                                     \
        REQUIRE_MESSAGE(exec.getStatus() == 0, exec.getError());                                                       \
    } while (false)

// A host VM that stays open between scripts, so tests can force collections.
class Host {
public:
    Host() { loader_.install(vm_); }

    // Runs a chunk; returns the error message, or "" on success.
    std::string run(std::string_view code) {
        lua_State* L = vm_.state();
        const sonata::luau::Bytecode bytecode = compiler_.compile(withLuau(code));
        int status = luau_load(L, "=test", bytecode.luauData(), bytecode.size(), 0);
        if (status == 0) {
            status = lua_pcall(L, 0, 0, 0);
        }
        if (status == 0) {
            return "";
        }
        const char* message = lua_tostring(L, -1);
        std::string error = message ? message : "(error value is not a string)";
        lua_pop(L, 1);
        return error;
    }

    void collect() {
        lua_gc(vm_.state(), LUA_GCCOLLECT, 0);
        lua_gc(vm_.state(), LUA_GCCOLLECT, 0);
    }

private:
    sonata::luau::ModuleLoader loader_{std::make_unique<sonata::luau::MemorySource>()};
    sonata::luau::VM vm_;
    sonata::luau::Compiler compiler_;
};

} // namespace

TEST_SUITE("luau library") {

    // in this VM ///////////////////////////////////////////////////////////

    TEST_CASE("luau exposes its version and the standard libraries") {
        RUN_LUAU(R"lua(
        assert(typeof(luau.version) == "string")
        local names = {}
        for _, name in luau.libraries do names[name] = true end
        assert(names.math and names.string and names.table and names.coroutine and names.buffer)
        assert(table.isfrozen(luau.libraries))
    )lua");
    }

    TEST_CASE("luau.check and luau.compile report syntax errors") {
        RUN_LUAU(R"lua(
        assert(luau.check("return 1 + 2") == true)
        local ok, message = luau.check("return +", "broken")
        assert(ok == false and string.find(message, "^broken:1:"), message)

        local bytecode = luau.compile("return ...", { optimizationLevel = 2, debugLevel = 0 })
        assert(typeof(bytecode) == "buffer" and buffer.len(bytecode) > 0)

        local compiled, err = pcall(luau.compile, "local = 1", { name = "bad" })
        assert(not compiled and string.find(err, "bad:1:"), err)
        assert(not pcall(luau.compile, "return 1", { optimizationLevel = 3 }))
        assert(not pcall(luau.compile, "return 1", { nope = true }))
    )lua");
    }

    TEST_CASE("luau.load runs chunks with their own globals") {
        RUN_LUAU(R"lua(
        -- Without env: reads through to our globals, writes stay in the chunk.
        local f = luau.load("leaked = 1; return math.floor(2.5), leaked")
        local a, b = f()
        assert(a == 2 and b == 1)
        assert(leaked == nil)

        -- With env: that table is the chunk's whole world.
        local env = { x = 20 }
        local g = assert(luau.load("y = x + 1; return math", { env = env }))
        assert(g() == nil and env.y == 21)

        -- Arguments arrive as "...".
        assert(luau.load("return select('#', ...), ...")(1, 2, 3) == 3)

        -- Syntax errors are returned, not raised.
        local fn, message = luau.load("return )", { name = "mine" })
        assert(fn == nil and string.find(message, "^mine:1:"), message)

        -- Runtime errors carry the chunk name.
        local ok, err = pcall(luau.load("error('boom')", { name = "thrower" }))
        assert(not ok and string.find(err, "thrower:1: boom"), err)

        -- Bytecode from luau.compile loads like source.
        local bytecode = luau.compile("return ... * 2")
        assert(luau.load(bytecode)(21) == 42)

        assert(not pcall(luau.load, 42))
        assert(not pcall(luau.load, "return 1", { env = 5 }))
    )lua");
    }

    TEST_CASE("luau.eval runs source and passes values through") {
        RUN_LUAU(R"lua(
        assert(luau.eval("return 1 + 2") == 3)
        local a, b = luau.eval("return ...", nil, "x", "y")
        assert(a == "x" and b == "y")
        assert(luau.eval("return value * 2", { value = 21 }) == 42)

        local ok, err = pcall(luau.eval, "return (")
        assert(not ok and string.find(err, "eval:1:"), err)
        ok, err = pcall(luau.eval, "error('inside', 0)")
        assert(not ok and err == "inside", err)
        assert(not pcall(luau.eval, 5))
        assert(not pcall(luau.eval, "return 1", 5))

        -- Evaluated code may yield: eval itself is plain Luau.
        local co = coroutine.wrap(function()
            return luau.eval("local got = coroutine.yield(1) return got + 1")
        end)
        assert(co() == 1)
        assert(co(41) == 42)
    )lua");
    }

    TEST_CASE("luau.env builds a sandboxed global table") {
        RUN_LUAU(R"lua(
        local env = luau.env()
        assert(env._G == env)
        assert(env.math == math and env.string == string and env.print == print and env.pcall == pcall)
        assert(env.require == nil and env.getfenv == nil and env.setfenv == nil and env.debug == nil)
        assert(env.luau == nil and env.sonata == nil)

        local f = luau.load("return typeof(require), typeof(string.format)", { env = env })
        local requireType, formatType = f()
        assert(requireType == "nil" and formatType == "function")

        local only = luau.env({ libraries = { "debug" }, globals = { answer = 42 } })
        assert(only.debug == debug and only.math == nil and only.answer == 42 and only.assert == assert)

        assert(not pcall(luau.env, { libraries = { "io" } }))
        assert(not pcall(luau.env, { globals = { [1] = true } }))
        assert(not pcall(luau.env, { unknown = 1 }))
    )lua");
    }

    // isolated VMs /////////////////////////////////////////////////////////

    TEST_CASE("A VM runs chunks and keeps its globals between them") {
        RUN_LUAU(R"lua(
        local vm = luau.vm()
        assert(typeof(vm) == "LuauVM" and string.find(tostring(vm), "LuauVM%(vm%)"))
        assert(vm:run("return 1 + 1") == 2)
        assert(select("#", vm:run("return")) == 0)

        vm:run("counter = 10")
        assert(vm:run("counter += 1; return counter") == 11)

        local a, b, c = vm:run("return ...", 1, "two", true)
        assert(a == 1 and b == "two" and c == true)

        -- Nothing leaks either way.
        assert(vm:run("return sonata == nil and args == nil and luau == nil"))
        assert(counter == nil)

        local names = vm:globals()
        local found = {}
        for _, name in names do found[name] = true end
        assert(found.counter and found.math and found.print)

        -- Bytecode runs too.
        assert(vm:run(luau.compile("return 'compiled'")) == "compiled")
    )lua");
    }

    TEST_CASE("VM values are copied, keeping cycles and shared tables") {
        RUN_LUAU(R"lua(
        local vm = luau.vm()

        local t = { list = { 1, 2, 3 }, name = "x" }
        vm:set("data", t)
        t.name = "changed"
        assert(vm:get("data.name") == "x")
        assert(vm:run("return #data.list") == 3)

        local back = vm:get("data")
        assert(back ~= t and back.list[2] == 2)

        local cyclic = {}
        cyclic.self = cyclic
        local shared = {}
        vm:set("c", { cyclic = cyclic, a = shared, b = shared })
        assert(vm:run("return c.cyclic.self == c.cyclic and c.a == c.b"))

        local out = vm:run("local r = {} r.me = r return r")
        assert(out.me == out)

        vm:set("frozen", table.freeze({ 1 }))
        assert(vm:run("return table.isfrozen(frozen)"))

        vm:set("bytes", buffer.fromstring("hi"))
        assert(vm:run("return buffer.tostring(bytes)") == "hi")

        local v = vm:run("return vector.create(1, 2, 3)")
        assert(typeof(v) == "vector" and v.x == 1 and v.z == 3)

        vm:set("tbl", { [true] = "yes", [2.5] = "float" })
        assert(vm:run("return tbl[true] == 'yes' and tbl[2.5] == 'float'"))

        local ok, err = pcall(vm.set, vm, "co", coroutine.create(print))
        assert(not ok and string.find(err, "cannot pass a thread value into the VM"), err)
        ok, err = pcall(vm.run, vm, "return coroutine.create(print)")
        assert(not ok and string.find(err, "cannot pass a thread value out of the VM"), err)
    )lua");
    }

    TEST_CASE("VM paths: get, set and call") {
        RUN_LUAU(R"lua(
        local vm = luau.vm()
        vm:run([[
            config = { debug = false, nested = { level = 1 } }
            api = { math = { add = function(a, b) return a + b end } }
            function greet(name) return "hello " .. name end
        ]])

        assert(vm:get("config.nested.level") == 1)
        assert(vm:get("missing") == nil and vm:get("missing.deeper") == nil)
        vm:set("config.debug", true)
        assert(vm:run("return config.debug") == true)
        vm:set("fresh", 5)
        assert(vm:run("return fresh") == 5)
        vm:set("fresh", nil)
        assert(vm:run("return fresh") == nil)

        assert(vm:call("greet", "you") == "hello you")
        assert(vm:call("api.math.add", 2, 3) == 5)
        assert(vm:call("string.upper", "abc") == "ABC")

        local ok, err = pcall(vm.set, vm, "nothing.here", 1)
        assert(not ok and string.find(err, "'nothing' is not a table"), err)
        ok, err = pcall(vm.call, vm, "nope")
        assert(not ok and string.find(err, "no function 'nope'"), err)
        ok, err = pcall(vm.get, vm, ".x")
        assert(not ok and string.find(err, "invalid path"), err)
    )lua");
    }

    TEST_CASE("Functions cross VMs as proxies and keep their identity") {
        RUN_LUAU(R"lua(
        local vm = luau.vm()
        local calls = 0
        local function add(a, b) calls += 1 return a + b, "sum" end
        vm:set("add", add)
        vm:set("again", add)
        local result, label = vm:run("return add(2, 3)")
        assert(result == 5 and label == "sum" and calls == 1)
        assert(vm:run("return add == again"))
        assert(vm:get("add") == add)

        -- Guest functions are callable from the host...
        local square = vm:run("return function(n) return n * n end")
        assert(square(9) == 81)
        -- ...even as callbacks of the host's own functions...
        local list = { 3, 1, 2 }
        table.sort(list, vm:run("return function(a, b) return a > b end"))
        assert(list[1] == 3 and list[3] == 1)
        -- ...and turn back into themselves when they return to their VM.
        vm:run("function original() end")
        vm:set("copy", vm:get("original"))
        assert(vm:run("return copy == original"))

        -- Functions inside tables cross too.
        vm:set("lib", { twice = function(x) return x * 2 end })
        assert(vm:run("return lib.twice(21)") == 42)

        -- Host -> VM -> host -> VM nesting.
        vm:set("inner", function() return vm:run("return 40") + 2 end)
        assert(vm:run("return inner()") == 42)

        -- A guest function given to another VM calls back into the first.
        local other = luau.vm()
        other:set("fromFirst", square)
        assert(other:run("return fromFirst(5)") == 25)
        other:close()
        vm:close() -- "inner" refers to vm, so only close() releases it
    )lua");
    }

    TEST_CASE("VM errors: run raises, try describes") {
        RUN_LUAU(R"lua(
        local vm = luau.vm({ name = "box" })

        local ok, err = pcall(vm.run, vm, "error('boom')")
        assert(not ok and string.find(err, "^box:1: boom"), err)

        ok, err = pcall(vm.run, vm, "return (")
        assert(not ok and string.find(err, "^box:1:"), err)

        local success, a, b = vm:try("return 1, 2")
        assert(success == true and a == 1 and b == 2)

        local failed, info = vm:try("local x = nil; return x.field")
        assert(failed == false and info.kind == "runtime", info.kind)
        assert(string.find(info.message, "box:1:") and info.value == info.message)
        assert(typeof(info.traceback) == "string")

        failed, info = vm:try("return +")
        assert(not failed and info.kind == "syntax", info.kind)

        failed, info = vm:try("error({ code = 7 })")
        assert(not failed and info.kind == "runtime" and info.value.code == 7)
        assert(info.message == "(error object is a table value)", info.message)

        failed, info = vm:try("coroutine.yield()")
        assert(not failed and info.kind == "yield", info.kind)

        -- Host function errors reach the VM, where pcall can catch them.
        vm:set("fail", function() error("nope", 0) end)
        failed, info = vm:try("fail()")
        assert(not failed and info.message == "nope", info.message)
        local caught, message = vm:run("return pcall(fail)")
        assert(caught == false and message == "nope")

        -- Host functions must not yield.
        vm:set("yielder", function() coroutine.yield() end)
        failed, info = vm:try("yielder()")
        assert(not failed and string.find(info.message, "yielded"), info.message)

        -- A VM function's error reaches the host.
        local thrower = vm:run("return function() error('from vm', 0) end")
        ok, err = pcall(thrower)
        assert(not ok and err == "from vm", err)

        -- The VM is still usable.
        assert(vm:run("return 'fine'") == "fine")
        local stats = vm:stats()
        assert(stats.errors >= 5 and stats.calls >= 10)
    )lua");
    }

    TEST_CASE("VM timeouts stop runaway code, even code that catches errors") {
        RUN_LUAU(R"lua(
        local vm = luau.vm({ timeout = 0.05 })
        local ok, info = vm:try("while true do end")
        assert(not ok and info.kind == "timeout", info.kind)
        assert(string.find(info.message, "timed out"), info.message)

        ok, info = vm:try("while true do pcall(function() while true do end end) end")
        assert(not ok and info.kind == "timeout", info.kind)

        ok, info = vm:try([[
            while true do
                local co = coroutine.create(function() while true do end end)
                coroutine.resume(co)
            end
        ]])
        assert(not ok and info.kind == "timeout", info.kind)

        local raised, err = pcall(vm.run, vm, "local i = 0 repeat i += 1 until false")
        assert(not raised and string.find(err, "timed out"), err)

        -- Each call gets a fresh budget.
        assert(vm:run("local n = 0 for i = 1, 1000 do n += i end return n") == 500500)
        assert(vm:stats().timeouts == 4 and vm:stats().timeout == 0.05)

        vm:setTimeout(nil)
        assert(vm:stats().timeout == nil)
        assert(vm:run("local n = 0 for i = 1, 100000 do n += 1 end return n") == 100000)
        assert(not pcall(vm.setTimeout, vm, -1))
    )lua");
    }

    TEST_CASE("VM memory limits stop allocation and recover") {
        RUN_LUAU(R"lua(
        local vm = luau.vm({ memoryLimit = 2 * 1024 * 1024 })
        assert(vm:stats().memoryLimit == 2 * 1024 * 1024)
        assert(vm:stats().memory > 0)

        local ok, info = vm:try("local t = {} for i = 1, 1e8 do t[i] = i end")
        assert(not ok and info.kind == "memory", info.kind .. ": " .. info.message)
        assert(string.find(info.message, "memory limit"), info.message)

        -- Code inside the VM can catch it too.
        local caught, message = vm:run("return pcall(function() return string.rep('x', 64 * 1024 * 1024) end)")
        assert(caught == false and string.find(message, "not enough memory"), message)

        -- The VM recovers once the garbage is gone.
        assert(vm:run("local t = {} for i = 1, 1000 do t[i] = i end return #t") == 1000)
        assert(vm:stats().peakMemory >= vm:stats().memory)
        assert(vm:collect() > 0)

        vm:setMemoryLimit(nil)
        assert(vm:stats().memoryLimit == nil)
        assert(vm:run("return #string.rep('x', 4 * 1024 * 1024)") == 4 * 1024 * 1024)
    )lua");
    }

    TEST_CASE("VM sandboxing and library selection") {
        RUN_LUAU(R"lua(
        local vm = luau.vm()
        local ok, err = pcall(vm.run, vm, "math.pi = 3")
        assert(not ok and string.find(err, "readonly"), err)
        ok = pcall(vm.run, vm, "getmetatable('').__index = {}")
        assert(not ok)
        -- _G is the shared, writable global table.
        assert(vm:run("_G.viaG = 1; return viaG") == 1)

        local open = luau.vm({ sandbox = false })
        assert(open:run("math.custom = 1; return math.custom") == 1)

        local small = luau.vm({ libraries = { "math" } })
        assert(small:run("return math ~= nil and string == nil and table == nil and coroutine == nil"))
        assert(small:run("return ('abc'):upper()") == "ABC") -- string methods remain

        assert(not pcall(luau.vm, { libraries = { "io" } }))
        assert(not pcall(luau.vm, { sandbox = "yes" }))
        assert(not pcall(luau.vm, { timeout = -1 }))
        assert(not pcall(luau.vm, { bogus = true }))
        assert(not pcall(luau.vm, { globals = { [1] = "x" } }))
        assert(not pcall(luau.vm, { globals = { co = coroutine.create(print) } }))
    )lua");
    }

    TEST_CASE("VM globals option and print redirection") {
        Exec exec(withLuau(R"lua(
        local lines = {}
        local vm = luau.vm({
            globals = { greeting = "hi", log = function(s) table.insert(lines, "log:" .. s) end },
            print = function(line) table.insert(lines, line) end,
        })
        vm:run("print('a', 1, nil, true) log(greeting)")
        assert(lines[1] == "a\t1\tnil\ttrue", lines[1])
        assert(lines[2] == "log:hi", lines[2])

        local quiet = luau.vm({ print = false })
        quiet:run("print('should not appear')")

        local loud = luau.vm()
        loud:run("print('from the vm')")
    )lua"));
        REQUIRE_MESSAGE(exec.getStatus() == 0, exec.getError());
        CHECK(exec.getOutput().find("should not appear") == std::string::npos);
        CHECK(exec.getOutput().find("from the vm") != std::string::npos);
    }

    TEST_CASE("VM modules work with require") {
        RUN_LUAU(R"lua(
        local vm = luau.vm({
            modules = {
                util = "return { twice = function(x) return x * 2 end }",
                a = "return require('b')",
                b = "return require('a')",
                broken = "error('module failed')",
                empty = "local x = 1",
            },
        })
        assert(vm:run("return require('util').twice(4)") == 8)
        assert(vm:run("return require('util') == require('util')"))
        assert(vm:run("return require('empty')") == true)

        local ok, err = pcall(vm.run, vm, "require('a')")
        assert(not ok and string.find(err, "cyclic require"), err)
        ok, err = pcall(vm.run, vm, "require('nowhere')")
        assert(not ok and string.find(err, "not found"), err)
        ok, err = pcall(vm.run, vm, "require('broken')")
        assert(not ok and string.find(err, "module failed"), err)
        -- A failed module can be required again (and fails again).
        ok, err = pcall(vm.run, vm, "require('broken')")
        assert(not ok and string.find(err, "module failed"), err)

        vm:defineModule("late", "return 'defined later'")
        assert(vm:run("return require('late')") == "defined later")
        ok, err = pcall(vm.defineModule, vm, "bad", "return (")
        assert(not ok and string.find(err, "bad:1:"), err)
        assert(not pcall(luau.vm, { modules = { broken = "return )" } }))
    )lua");
    }

    TEST_CASE("vm:load gives a reusable function") {
        RUN_LUAU(R"lua(
        local vm = luau.vm()
        local chunk = vm:load("total = (total or 0) + ...; return total", "adder")
        assert(chunk(5) == 5 and chunk(5) == 10)
        local ok, err = pcall(vm:load("error('x')", "named"))
        assert(not ok and string.find(err, "^named:1: x"), err)
        ok, err = pcall(vm.load, vm, "return (", "syntax")
        assert(not ok and string.find(err, "^syntax:1:"), err)
    )lua");
    }

    TEST_CASE("VM reset and close") {
        RUN_LUAU(R"lua(
        local vm = luau.vm({ globals = { base = 1 }, modules = { m = "return {}" } })
        vm:run("extra = 2")
        local old = vm:run("return function() return 'old' end")
        local module = vm:run("return require('m')")

        vm:reset()
        assert(vm:run("return base, extra") == 1)
        assert(select(2, vm:run("return base, extra")) == nil)
        assert(vm:run("return require('m')") ~= module)
        local ok, err = pcall(old)
        assert(not ok and string.find(err, "reset"), err)
        assert(vm:stats().calls == 3)

        -- A VM can't be closed or reset while it's running code.
        vm:set("closer", function() vm:close() end)
        ok, err = pcall(vm.run, vm, "closer()")
        assert(not ok and string.find(err, "cannot close a VM while it is running"), err)
        vm:set("resetter", function() vm:reset() end)
        ok, err = pcall(vm.run, vm, "resetter()")
        assert(not ok and string.find(err, "cannot reset"), err)

        local fn = vm:run("return function() return 1 end")
        assert(not vm:isClosed())
        vm:close()
        vm:close() -- closing twice is fine
        assert(vm:isClosed() and string.find(tostring(vm), "closed"))
        ok, err = pcall(vm.run, vm, "return 1")
        assert(not ok and string.find(err, "closed"), err)
        ok, err = pcall(fn)
        assert(not ok and string.find(err, "closed"), err)
        assert(not pcall(vm.get, vm, "x"))
        assert(not pcall(vm.globals, vm))
        assert(not pcall(vm.collect, vm))
    )lua");
    }

    TEST_CASE("Unbounded recursion across VMs ends in an error") {
        RUN_LUAU(R"lua(
        local vm = luau.vm()
        vm:set("down", function(n) return vm:call("up", n + 1) end)
        vm:run("function up(n) return down(n) end")
        local ok, err = pcall(vm.call, vm, "up", 0)
        assert(not ok and string.find(tostring(err), "overflow"), err)
        assert(vm:run("return 'still fine'") == "still fine")
        vm:close()
    )lua");
    }

    TEST_CASE("Collected VMs and proxies release their references") {
        Host host;
        // VMs and proxies of every kind, then dropped.
        REQUIRE(host.run(R"lua(
        for i = 1, 30 do
            local vm = luau.vm({ globals = { id = function() return i end }, print = function() end })
            vm:set("echo", function(x) return x end)
            local f = vm:load("return id() + echo(1)")
            assert(f() == i + 1)
            local g = vm:run("return function() return id() end")
            assert(g() == i)
            if i % 3 == 0 then vm:close() end
        end
        _G.kept = luau.vm()
        _G.keptFn = _G.kept:run("return function(x) return x end")
    )lua") == "");
        host.collect();

        // Every call flushes what the collection queued; nothing may break.
        REQUIRE(host.run(R"lua(
        local vm = luau.vm()
        for i = 1, 200 do
            vm:set("f" .. i % 5, function() return i end)
        end
        assert(vm:collect() > 0)
        assert(vm:run("return f1()") == 196)
        assert(_G.keptFn(7) == 7)
    )lua") == "");
        host.collect();

        REQUIRE(host.run(R"lua(
        _G.keptFn = nil
        _G.kept = nil
        local vm = luau.vm()
        assert(vm:run("return 1") == 1)
    )lua") == "");
        host.collect();
        REQUIRE(host.run("assert(luau.vm():run('return 2') == 2)") == "");
    }
}
