// @sonata/luau: compile, load and run Luau code, either inside the running VM
// or in isolated VMs with their own globals, memory limit and time limit.
//
//     local luau = require("@sonata/luau")
//
//   In this VM (values are shared by reference; no resource limits)
//     luau.compile(source, options?)   --> buffer     bytecode; raises on a syntax error
//     luau.check(source, name?)        --> true | false, message
//     luau.load(chunk, options?)       --> function | nil, message
//     luau.eval(source, env?, ...)     --> ...        load + call; raises
//     luau.env(options?)               --> table      a global table holding only safe builtins
//     luau.libraries                   --> { string } standard libraries a VM can be given
//     luau.version                     --> string     Luau's version ("" if unknown)
//
//     chunk:   source code, or a buffer made by luau.compile
//     options (compile/load): { name, optimizationLevel (0-2), debugLevel (0-2), env (load) }
//     luau.load without "env" gives the chunk a fresh global table that reads
//     through to the caller's globals, so the chunk's own globals stay in it.
//     luau.env options: { libraries = { string }?, globals = { [string]: any }? }
//
//   Isolated VMs
//     luau.vm(options?) --> VM
//         options: { name, libraries, sandbox = true, globals, memoryLimit (bytes),
//                    timeout (seconds per call), print = function | false,
//                    modules = { [name]: source }, optimizationLevel, debugLevel }
//     vm:run(chunk, ...)           --> ...          raises on failure
//     vm:try(chunk, ...)           --> true, ... | false, VMError
//     vm:load(chunk, name?)        --> function     runs the chunk in the VM on every call
//     vm:call(path, ...)           --> ...          path: "name" or "a.b.c"
//     vm:get(path)                 --> any
//     vm:set(path, value)
//     vm:globals()                 --> { string }
//     vm:defineModule(name, source)                 for require(name) inside the VM
//     vm:setTimeout(seconds?) / vm:setMemoryLimit(bytes?)
//     vm:stats()                   --> { memory, peakMemory, memoryLimit?, timeout?,
//                                        calls, errors, timeouts }
//     vm:collect()                 --> number       bytes in use after a full collection
//     vm:reset() / vm:close() / vm:isClosed()
//
//     VMError = { kind: "syntax" | "runtime" | "timeout" | "memory" | "yield",
//                 message: string, value: any, traceback: string }
//
// HOW AN ISOLATED VM WORKS
//
// A VM is a separate lua_State with its own heap (the "guest"); the script
// that made it is the "host". Nothing is shared: values crossing between them
// are copied (tables deeply, keeping cycles and shared references; metatables
// are not copied). Functions cross as proxies: calling one runs the original
// in its own VM, with the arguments and results copied. Userdata, threads and
// light userdata can't cross.
//
// Every call into the guest runs on a fresh guest thread. While it runs, the
// guest's allocator enforces the memory limit and its interrupt callback the
// deadline. Host functions the guest calls run on a fresh host thread, so the
// scheduler waits inline for anything they wait on (see scheduler.hpp).
// Passing the calling thread as lua_resume's "from" carries Luau's C-call
// counter across VMs, so runaway host <-> guest recursion ends in Luau's own
// "C stack overflow" error instead of overflowing the native stack.
//
// REFERENCES AND GARBAGE COLLECTION
//
// A host function given to the guest is anchored in the host registry; a
// guest function given to the host in the guest registry. Proxies release
// their anchor when collected, but a destructor runs in the middle of its
// VM's collection (or its lua_close), when touching that VM's heap is not
// allowed. So destructors only queue the reference, and the queue is flushed
// (lua_unref) the next time the host calls into this library. References of
// a VM that is collected go to a per-host "graveyard", flushed the same way.
//
// Only a host function that captures its own VM object keeps that VM alive
// for good (the cycle runs through the host registry); vm:close() ends it.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sonata/core/info.hpp>
#include <sonata/core/library.hpp>
#include <sonata/core/luau/compiler.hpp>
#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs {

namespace {

// Errors detected by this library's own checks. Bindings run their C++ work
// in guarded(), which raises a Failure as a Lua error only once every C++
// object of the binding is gone. Errors Luau raises itself are not caught.
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// fn returns its result count, or -1 with the error value on top of the stack.
template <typename F>
int guarded(lua_State* L, F&& fn) {
    int results = -1;
    try {
        results = fn();
    } catch (const Failure& e) {
        lua_pushstring(L, e.what());
    }

    if (results < 0) {
        lua_error(L);
    }
    return results;
}

constexpr const char* kVMMeta = "sonata.luau.VM";
constexpr const char* kGraveyardKey = "sonata.luau.graveyard";  // host registry
constexpr const char* kHostProxiesKey = "sonata.luau.hostProxies"; // guest registry
constexpr const char* kModulesKey = "sonata.luau.modules";         // guest registry

constexpr int kMaxCopyDepth = 100;
constexpr unsigned kTimeoutCheckInterval = 32; // interrupts between two clock reads

constexpr const char* kDefaultChunkName = "chunk";
constexpr const char* kDefaultVMName = "vm";

constexpr const char* kStandardLibraries[] = {
    "bit32", "buffer", "coroutine", "debug", "integer", "math", "os", "string", "table", "utf8", "vector",
};

// What luau.env() hands out besides the libraries. Left out on purpose:
// getfenv/setfenv (they reach the environment of any function on the call
// stack, the host's included), require, and Sonata's own globals.
constexpr const char* kSafeBaseFunctions[] = {
    "assert", "error",    "gcinfo", "getmetatable", "ipairs", "newproxy", "next",   "pairs",
    "pcall",  "print",    "rawequal", "rawget",     "rawlen", "rawset",   "select", "setmetatable",
    "tonumber", "tostring", "type", "typeof",       "unpack", "xpcall",   "_VERSION",
};

int libraryIndex(std::string_view name) {
    for (std::size_t i = 0; i < std::size(kStandardLibraries); ++i) {
        if (name == kStandardLibraries[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int absIndex(lua_State* L, int index) {
    return (index > 0 || index <= LUA_REGISTRYINDEX) ? index : lua_gettop(L) + index + 1;
}

// For values already known to be strings (no number conversion happens).
std::string stringAt(lua_State* L, int index) {
    std::size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    return text ? std::string(text, length) : std::string();
}

// options ///////////////////////////////////////////////////////////////////

// An optional options table. Every problem throws a Failure naming the
// function and the option. Reads are raw, so no metamethod ever runs.
class Options {
public:
    Options(lua_State* L, int index, const char* function, std::initializer_list<const char*> known)
        : L_(L), index_(absIndex(L, index)), function_(function) {
        const int type = lua_type(L, index_);
        if (type == LUA_TNONE || type == LUA_TNIL) {
            index_ = 0;
            return;
        }
        if (type != LUA_TTABLE) {
            throw Failure(function_ + ": options must be a table, got " + lua_typename(L, type));
        }

        lua_pushnil(L);
        while (lua_next(L, index_) != 0) {
            lua_pop(L, 1);
            if (lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 1);
                throw Failure(function_ + ": option names must be strings");
            }
            const std::string key = stringAt(L, -1);
            const bool isKnown = std::any_of(known.begin(), known.end(), [&](const char* k) { return key == k; });
            if (!isKnown) {
                lua_pop(L, 1);
                throw Failure(function_ + ": unknown option '" + key + "'");
            }
        }
    }

    // Pushes options[key] (nil without options) and returns its absolute index.
    int push(const char* key) const {
        if (index_ == 0) {
            lua_pushnil(L_);
        } else {
            lua_rawgetfield(L_, index_, key);
        }
        return lua_gettop(L_);
    }

    int type(const char* key) const {
        push(key);
        const int t = lua_type(L_, -1);
        lua_pop(L_, 1);
        return t;
    }

    std::optional<std::string> string(const char* key) const {
        const int t = type(key);
        if (t == LUA_TNIL) {
            return std::nullopt;
        }
        if (t != LUA_TSTRING) {
            fail(key, "a string");
        }
        push(key);
        std::string value = stringAt(L_, -1);
        lua_pop(L_, 1);
        return value;
    }

    std::optional<double> number(const char* key) const {
        const int t = type(key);
        if (t == LUA_TNIL) {
            return std::nullopt;
        }
        if (t != LUA_TNUMBER) {
            fail(key, "a number");
        }
        push(key);
        const double value = lua_tonumber(L_, -1);
        lua_pop(L_, 1);
        return value;
    }

    std::optional<bool> boolean(const char* key) const {
        const int t = type(key);
        if (t == LUA_TNIL) {
            return std::nullopt;
        }
        if (t != LUA_TBOOLEAN) {
            fail(key, "a boolean");
        }
        push(key);
        const bool value = lua_toboolean(L_, -1) != 0;
        lua_pop(L_, 1);
        return value;
    }

    // optimizationLevel / debugLevel: 0, 1 or 2.
    int level(const char* key, int fallback) const {
        const std::optional<double> value = number(key);
        if (!value) {
            return fallback;
        }
        if (*value != 0 && *value != 1 && *value != 2) {
            fail(key, "0, 1 or 2");
        }
        return static_cast<int>(*value);
    }

    std::optional<std::vector<std::string>> strings(const char* key) const {
        const int t = type(key);
        if (t == LUA_TNIL) {
            return std::nullopt;
        }
        if (t != LUA_TTABLE) {
            fail(key, "an array of strings");
        }

        const int list = push(key);
        std::vector<std::string> values;
        const int count = lua_objlen(L_, list);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L_, list, i);
            if (lua_type(L_, -1) != LUA_TSTRING) {
                lua_pop(L_, 2);
                fail(key, "an array of strings");
            }
            values.push_back(stringAt(L_, -1));
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
        return values;
    }

    [[noreturn]] void fail(const char* key, const char* expected) const {
        throw Failure(function_ + ": option '" + key + "' must be " + expected);
    }

    const std::string& function() const { return function_; }

private:
    lua_State* L_;
    int index_;
    std::string function_;
};

// chunks ////////////////////////////////////////////////////////////////////

struct Levels {
    int optimization = 1;
    int debug = 1;
};

Levels readLevels(const Options& options) {
    return {options.level("optimizationLevel", 1), options.level("debugLevel", 1)};
}

// A chunk argument: source text, or bytecode in a buffer. Points into the Lua
// value, which must stay on the stack.
struct Chunk {
    const char* data = nullptr;
    std::size_t size = 0;
    bool bytecode = false;
};

Chunk checkChunk(lua_State* L, int index) {
    Chunk chunk;
    if (lua_type(L, index) == LUA_TBUFFER) {
        chunk.data = static_cast<const char*>(lua_tobuffer(L, index, &chunk.size));
        chunk.bytecode = true;
    } else if (lua_type(L, index) == LUA_TSTRING) {
        chunk.data = lua_tolstring(L, index, &chunk.size);
    } else {
        luaL_typeerror(L, index, "string or buffer");
    }
    return chunk;
}

// Source -> bytecode. A syntax error is not an exception: it returns false
// with "<name>:<line>: <message>" in "error".
bool toBytecode(const Chunk& chunk, Levels levels, const std::string& name, std::string& bytecode,
                std::string& error) {
    if (chunk.bytecode) {
        bytecode.assign(chunk.data, chunk.size);
        return true;
    }

    sonata::luau::Compiler::Options options;
    options.optimizationLevel = levels.optimization;
    options.debugLevel = levels.debug;
    const sonata::luau::Bytecode result =
        sonata::luau::Compiler(options).compile(std::string_view(chunk.data, chunk.size));

    if (std::optional<std::string> message = result.compileError()) {
        error = name + *message;
        return false;
    }

    bytecode.assign(result.luauData(), result.size());
    return true;
}

// "=name": Luau shows such chunk names as they are ("name:3: ...").
std::string chunkName(const std::string& name) { return "=" + name; }

// the sandbox ///////////////////////////////////////////////////////////////

// Host registry references to release, from VMs that were collected.
struct Graveyard {
    std::vector<int> refs;
};

struct Allocator {
    std::size_t used = 0;
    std::size_t peak = 0;
    std::size_t limit = 0; // 0 = none
    bool enforcing = false; // only while VM code runs: the host's own copying never fails
    bool hitLimit = false;
};

void* allocate(void* ud, void* block, std::size_t oldSize, std::size_t newSize) {
    auto* allocator = static_cast<Allocator*>(ud);

    if (newSize == 0) {
        std::free(block);
        allocator->used -= oldSize;
        return nullptr;
    }

    if (allocator->enforcing && allocator->limit != 0 && newSize > oldSize &&
        allocator->used - oldSize + newSize > allocator->limit) {
        allocator->hitLimit = true;
        return nullptr; // Luau raises "not enough memory"
    }

    void* result = std::realloc(block, newSize);
    if (result == nullptr) {
        return nullptr;
    }

    allocator->used = allocator->used - oldSize + newSize;
    allocator->peak = std::max(allocator->peak, allocator->used);
    return result;
}

struct Module {
    std::string chunkName;
    std::string bytecode;
};

enum class PrintMode { Default, Silent, Host };

struct VMOptions {
    std::string name = kDefaultVMName;
    std::vector<std::string> libraries;
    bool allLibraries = true;
    bool sandbox = true;
    double timeout = 0; // seconds; 0 = none
    PrintMode print = PrintMode::Default;
    Levels levels;
    std::map<std::string, Module, std::less<>> modules;
};

struct Sandbox {
    lua_State* G = nullptr; // the guest VM; nullptr once closed
    Allocator alloc;        // must outlive G
    VMOptions options;
    std::weak_ptr<Sandbox> self;
    std::shared_ptr<Graveyard> graveyard; // of the host VM
    std::uint64_t generation = 0;         // bumped by every close: older guest refs are void

    int envRef = LUA_NOREF;     // guest: the table chunks run in
    int globalsRef = LUA_NOREF; // host: options.globals
    int printRef = LUA_NOREF;   // host: options.print

    // Set while VM code runs.
    lua_State* host = nullptr; // the host thread that called in; host functions run from it
    int depth = 0;             // nested calls into the guest
    double deadline = 0;       // lua_clock() time; 0 = none
    double budget = 0;         // the timeout the deadline came from
    unsigned ticks = 0;
    bool timedOut = false;
    bool closing = false;

    // Host functions anchored for the guest, by host ref. "proxies" counts the
    // guest proxies alive for it (plus reservations, see pushHostProxy()).
    struct HostFunction {
        const void* pointer;
        int proxies;
    };
    std::unordered_map<int, HostFunction> hostFunctions;
    std::unordered_map<const void*, int> hostFunctionRefs;

    std::vector<int> deadHostRefs;  // to lua_unref on the host
    std::vector<int> deadGuestRefs; // to lua_unref on the guest

    std::uint64_t calls = 0;
    std::uint64_t errors = 0;
    std::uint64_t timeouts = 0;

    Sandbox() = default;
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    ~Sandbox();
};

Sandbox& sandboxOf(lua_State* L) { return *static_cast<Sandbox*>(lua_callbacks(L)->userdata); }

std::shared_ptr<Graveyard>* graveyardBox(lua_State* L) {
    lua_rawgetfield(L, LUA_REGISTRYINDEX, kGraveyardKey);
    auto* box = static_cast<std::shared_ptr<Graveyard>*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return box;
}

void flushGraveyard(lua_State* L) {
    std::shared_ptr<Graveyard>* box = graveyardBox(L);
    if (box == nullptr || !*box) {
        return;
    }
    for (int ref : (*box)->refs) {
        lua_unref(L, ref);
    }
    (*box)->refs.clear();
}

// Closes the guest VM. Host refs it held become dead (released by
// releaseHostRefs()); its own refs vanish with it.
void closeGuest(Sandbox& sb) {
    if (sb.G == nullptr) {
        return;
    }

    sb.closing = true; // proxy destructors run now: nothing to keep track of any more
    lua_close(sb.G);
    sb.closing = false;

    sb.G = nullptr;
    sb.envRef = LUA_NOREF;
    ++sb.generation;
    sb.deadGuestRefs.clear();

    for (const auto& entry : sb.hostFunctions) {
        sb.deadHostRefs.push_back(entry.first);
    }
    sb.hostFunctions.clear();
    sb.hostFunctionRefs.clear();
}

// Releases the dead host refs (and with "everything", the options' refs too):
// right away with a host thread, or through the graveyard without one.
void releaseHostRefs(Sandbox& sb, lua_State* H, bool everything) {
    if (everything) {
        for (int* ref : {&sb.globalsRef, &sb.printRef}) {
            if (*ref != LUA_NOREF) {
                sb.deadHostRefs.push_back(*ref);
                *ref = LUA_NOREF;
            }
        }
    }

    if (H != nullptr) {
        for (int ref : sb.deadHostRefs) {
            lua_unref(H, ref);
        }
    } else if (sb.graveyard) {
        sb.graveyard->refs.insert(sb.graveyard->refs.end(), sb.deadHostRefs.begin(), sb.deadHostRefs.end());
    }
    sb.deadHostRefs.clear();
}

Sandbox::~Sandbox() {
    closeGuest(*this);
    releaseHostRefs(*this, nullptr, true);
}

// Releases what proxies queued since the last call. "H" is a host thread.
void flush(lua_State* H, Sandbox& sb) {
    flushGraveyard(H);
    releaseHostRefs(sb, H, false);

    if (sb.G != nullptr) {
        for (int ref : sb.deadGuestRefs) {
            lua_unref(sb.G, ref);
        }
    }
    sb.deadGuestRefs.clear();
}

void guestInterrupt(lua_State* L, int gc) {
    if (gc >= 0) {
        return; // a GC step, not a safepoint in code
    }

    Sandbox* sb = static_cast<Sandbox*>(lua_callbacks(L)->userdata);
    if (sb == nullptr || sb->deadline <= 0) {
        return;
    }

    if (!sb->timedOut) {
        if (++sb->ticks % kTimeoutCheckInterval != 0 || lua_clock() < sb->deadline) {
            return;
        }
        sb->timedOut = true;
    }

    // Raised at every safepoint from now on, so catching it with pcall only
    // gets the code as far as its next loop iteration, call or return.
    luaL_error(L, "timed out after %g seconds", sb->budget);
}

void applyTimeout(Sandbox& sb) {
    if (sb.G != nullptr) {
        lua_callbacks(sb.G)->interrupt = sb.options.timeout > 0 ? guestInterrupt : nullptr;
    }
}

// proxies ///////////////////////////////////////////////////////////////////

// The upvalue of a guest closure that calls a host function.
struct HostProxy {
    Sandbox* sandbox;
    int ref; // host registry
};

// The upvalue of a host closure that calls a guest function.
struct GuestProxy {
    std::shared_ptr<Sandbox> sandbox;
    std::uint64_t generation;
    int ref; // guest registry
};

int callHost(lua_State* L);
int callGuest(lua_State* L);

// Destructors run inside a collection: they only update bookkeeping.
void destroyHostProxy(lua_State*, void* memory) {
    const auto* proxy = static_cast<HostProxy*>(memory);
    Sandbox* sb = proxy->sandbox;
    if (sb == nullptr || sb->closing) {
        return;
    }

    const auto it = sb->hostFunctions.find(proxy->ref);
    if (it == sb->hostFunctions.end() || --it->second.proxies > 0) {
        return;
    }

    try {
        sb->deadHostRefs.push_back(proxy->ref);
    } catch (...) {
        return; // the ref leaks until the VM closes, which is harmless
    }
    sb->hostFunctionRefs.erase(it->second.pointer);
    sb->hostFunctions.erase(it);
}

void destroyGuestProxy(lua_State*, void* memory) {
    auto* proxy = static_cast<GuestProxy*>(memory);
    Sandbox* sb = proxy->sandbox.get();

    if (sb != nullptr && sb->G != nullptr && sb->generation == proxy->generation) {
        try {
            sb->deadGuestRefs.push_back(proxy->ref);
        } catch (...) {
        }
    }

    proxy->~GuestProxy(); // may be the last owner: the VM closes here
}

// Anchors the host function at "index" of H and returns its ref, with one
// proxy reserved, so that no collection can release the ref before the
// caller has made (or found) the proxy that will own it.
int reserveHostFunction(lua_State* H, int index, Sandbox& sb) {
    const void* pointer = lua_topointer(H, index);

    if (const auto found = sb.hostFunctionRefs.find(pointer); found != sb.hostFunctionRefs.end()) {
        ++sb.hostFunctions.at(found->second).proxies;
        return found->second;
    }

    const int ref = lua_ref(H, index);
    sb.hostFunctions.emplace(ref, Sandbox::HostFunction{pointer, 1});
    sb.hostFunctionRefs.emplace(pointer, ref);
    return ref;
}

// Pushes onto T (a guest thread) the guest function standing for host ref
// "ref", reusing a live proxy when there is one so the function keeps its
// identity. Consumes the reservation made by reserveHostFunction().
void pushHostProxy(lua_State* T, Sandbox& sb, int ref) {
    lua_rawgetfield(T, LUA_REGISTRYINDEX, kHostProxiesKey);
    lua_rawgeti(T, -1, ref);

    if (lua_isfunction(T, -1)) {
        lua_remove(T, -2);
        --sb.hostFunctions.at(ref).proxies; // that proxy already counts
        return;
    }
    lua_pop(T, 1);

    auto* proxy = static_cast<HostProxy*>(lua_newuserdatadtor(T, sizeof(HostProxy), destroyHostProxy));
    proxy->sandbox = &sb;
    proxy->ref = ref; // takes over the reservation

    lua_pushcclosure(T, callHost, "host", 1);
    lua_pushvalue(T, -1);
    lua_rawseti(T, -3, ref);
    lua_remove(T, -2);
}

// Pushes onto H a host function that calls the guest function at "index" of T.
void pushGuestProxy(lua_State* H, lua_State* T, int index, Sandbox& sb) {
    const int ref = lua_ref(T, index);

    void* memory = lua_newuserdatadtor(H, sizeof(GuestProxy), destroyGuestProxy);
    new (memory) GuestProxy{sb.self.lock(), sb.generation, ref};

    lua_pushcclosure(H, callGuest, "vm", 1);
}

// copying ///////////////////////////////////////////////////////////////////

// Copies values from one VM to the other. Never raises on "from"; raises on
// "to" only if it runs out of memory.
class Copier {
public:
    Copier(Sandbox& sb, lua_State* from, lua_State* to, bool intoGuest)
        : sb_(sb), from_(from), to_(to), intoGuest_(intoGuest) {}

    // Pushes copies of from[first .. first + count - 1] onto "to". On failure
    // "to" is left as it was and error() says why.
    bool copyRange(int first, int count) {
        first = absIndex(from_, first);
        const int base = lua_gettop(to_);

        for (int i = 0; i < count; ++i) {
            if (!copyValue(first + i, 0)) {
                lua_settop(to_, base); // also drops the "seen" table
                seen_ = 0;
                return false;
            }
        }

        if (seen_ != 0) {
            lua_remove(to_, seen_);
            seen_ = 0;
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
        return false;
    }

    const char* direction() const { return intoGuest_ ? "into the VM" : "out of the VM"; }

    bool copyValue(int index, int depth) {
        if (!lua_checkstack(to_, 4) || !lua_checkstack(from_, 4)) {
            return fail("value is nested too deeply to copy");
        }

        const int type = lua_type(from_, index);
        switch (type) {
        case LUA_TNONE:
        case LUA_TNIL:
            lua_pushnil(to_);
            return true;
        case LUA_TBOOLEAN:
            lua_pushboolean(to_, lua_toboolean(from_, index));
            return true;
        case LUA_TNUMBER:
            lua_pushnumber(to_, lua_tonumber(from_, index));
            return true;
        case LUA_TINTEGER: {
            int isInteger = 0;
            lua_pushinteger64(to_, lua_tointeger64(from_, index, &isInteger));
            return true;
        }
        case LUA_TVECTOR: {
            const LUA_VECTOR_TYPE* v = lua_tovector(from_, index);
#if LUA_VECTOR_SIZE == 4
            lua_pushvector(to_, v[0], v[1], v[2], v[3]);
#else
            lua_pushvector(to_, v[0], v[1], v[2]);
#endif
            return true;
        }
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(from_, index, &length);
            lua_pushlstring(to_, text, length);
            return true;
        }
        case LUA_TBUFFER: {
            std::size_t length = 0;
            const void* source = lua_tobuffer(from_, index, &length);
            void* target = lua_newbuffer(to_, length);
            if (length != 0) {
                std::memcpy(target, source, length);
            }
            return true;
        }
        case LUA_TTABLE:
            return copyTable(index, depth);
        case LUA_TFUNCTION:
            return copyFunction(index);
        default:
            return fail(std::string("cannot pass a ") + lua_typename(from_, type) + " value " + direction());
        }
    }

    bool copyTable(int index, int depth) {
        if (depth >= kMaxCopyDepth) {
            return fail("tables are nested too deeply to copy");
        }

        if (seen_ == 0) { // source table -> its copy, for cycles and shared references
            lua_createtable(to_, 0, 0);
            seen_ = lua_gettop(to_);
        }

        void* key = const_cast<void*>(lua_topointer(from_, index));
        lua_pushlightuserdata(to_, key);
        lua_rawget(to_, seen_);
        if (!lua_isnil(to_, -1)) {
            return true;
        }
        lua_pop(to_, 1);

        lua_createtable(to_, lua_objlen(from_, index), 0);
        const int copy = lua_gettop(to_);
        lua_pushlightuserdata(to_, key);
        lua_pushvalue(to_, copy);
        lua_rawset(to_, seen_);

        lua_pushnil(from_);
        while (lua_next(from_, index) != 0) {
            const int value = lua_gettop(from_);
            if (!copyValue(value - 1, depth + 1) || !copyValue(value, depth + 1)) {
                lua_pop(from_, 2);
                lua_settop(to_, copy);
                return false;
            }
            lua_rawset(to_, copy);
            lua_pop(from_, 1);
        }

        if (lua_getreadonly(from_, index)) {
            lua_setreadonly(to_, copy, 1);
        }
        return true;
    }

    // The upvalue of the proxy at "index" if it is one made with "function".
    const void* proxyAt(int index, lua_CFunction function) {
        if (lua_tocfunction(from_, index) != function || lua_getupvalue(from_, index, 1) == nullptr) {
            return nullptr;
        }
        const void* proxy = lua_touserdata(from_, -1);
        lua_pop(from_, 1);
        return proxy;
    }

    bool copyFunction(int index) {
        // A function going back to the VM it came from: hand over the original.
        if (intoGuest_) {
            const auto* proxy = static_cast<const GuestProxy*>(proxyAt(index, callGuest));
            if (proxy != nullptr && proxy->sandbox.get() == &sb_ && proxy->generation == sb_.generation) {
                lua_getref(to_, proxy->ref);
                return true;
            }

            pushHostProxy(to_, sb_, reserveHostFunction(from_, index, sb_));
            return true;
        }

        const auto* proxy = static_cast<const HostProxy*>(proxyAt(index, callHost));
        if (proxy != nullptr && proxy->sandbox == &sb_) {
            lua_getref(to_, proxy->ref);
            return true;
        }

        pushGuestProxy(to_, from_, index, sb_);
        return true;
    }

    Sandbox& sb_;
    lua_State* from_;
    lua_State* to_;
    bool intoGuest_;
    int seen_ = 0; // index on "to"
    std::string error_;
};

// Pushes onto "to" the error value on top of "from": strings as they are,
// anything else copied, or described if it can't be.
void pushErrorValue(Sandbox& sb, lua_State* from, lua_State* to, bool intoGuest) {
    const int type = lua_type(from, -1);

    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(from, -1, &length);
        lua_pushlstring(to, text, length);
        return;
    }

    Copier copier(sb, from, to, intoGuest);
    if (type != LUA_TNONE && copier.copyRange(-1, 1)) {
        return;
    }

    lua_pushfstring(to, "(error object is a %s value)", lua_typename(from, type));
}

bool isOutOfMemoryMessage(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TSTRING) {
        return false;
    }
    constexpr std::string_view suffix = "not enough memory";
    std::size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    const std::string_view message(text, length);
    return message.size() >= suffix.size() && message.substr(message.size() - suffix.size()) == suffix;
}

// calling across ////////////////////////////////////////////////////////////

// Calls host function "ref" for guest thread L with L's values first ..
// first + count - 1. Pushes the results onto L and returns how many, or
// pushes the error value and returns -1.
int invokeHost(lua_State* L, Sandbox& sb, int ref, int first, int count) {
    lua_State* H = sb.host;
    if (H == nullptr) {
        lua_pushstring(L, "host functions can only be called while the host runs VM code");
        return -1;
    }
    if (!lua_checkstack(H, 2)) {
        lua_pushstring(L, "host stack overflow");
        return -1;
    }

    const int hostTop = lua_gettop(H);
    lua_State* R = lua_newthread(H); // stays anchored on H's stack until the end
    lua_getref(R, ref);

    const bool enforcing = sb.alloc.enforcing;
    sb.alloc.enforcing = false; // results the host hands over are the host's doing
    int results = -1;

    if (!lua_isfunction(R, -1)) {
        lua_pushstring(L, "host function is no longer available");
    } else if (!lua_checkstack(R, count + 1)) {
        lua_pushstring(L, "too many arguments for a host function");
    } else {
        Copier arguments(sb, L, R, false);
        if (!arguments.copyRange(first, count)) {
            lua_pushstring(L, arguments.error().c_str());
        } else {
            const int status = lua_resume(R, L, count);

            if (status == LUA_OK) {
                const int n = lua_gettop(R);
                Copier copier(sb, R, L, true);
                if (!lua_checkstack(L, n + 1)) {
                    lua_pushstring(L, "too many results from a host function");
                } else if (!copier.copyRange(1, n)) {
                    lua_pushstring(L, copier.error().c_str());
                } else {
                    results = n;
                }
            } else if (status == LUA_YIELD) {
                lua_pushstring(L, "a host function called from a VM yielded");
            } else {
                pushErrorValue(sb, R, L, true);
            }
        }
    }

    sb.alloc.enforcing = enforcing;
    lua_settop(H, hostTop);
    return results;
}

// A host function, as the guest sees it.
int callHost(lua_State* L) {
    const auto* proxy = static_cast<const HostProxy*>(lua_touserdata(L, lua_upvalueindex(1)));
    const int results = invokeHost(L, *proxy->sandbox, proxy->ref, 1, lua_gettop(L));
    if (results < 0) {
        lua_error(L);
    }
    return results;
}

enum class Kind { Ok, Syntax, Runtime, Timeout, Memory, Yield };

const char* kindName(Kind kind) {
    switch (kind) {
    case Kind::Ok:
        return "ok";
    case Kind::Syntax:
        return "syntax";
    case Kind::Runtime:
        return "runtime";
    case Kind::Timeout:
        return "timeout";
    case Kind::Memory:
        return "memory";
    case Kind::Yield:
        return "yield";
    }
    return "runtime";
}

// How a call into the guest went. With Kind::Ok, "results" values are on
// the host's stack; otherwise one error value is.
struct Outcome {
    Kind kind = Kind::Ok;
    int results = 0;
    std::string traceback;
};

// Runs something in the guest for host thread H. "setup" pushes onto the
// fresh guest thread what to call (and any fixed arguments) and returns how
// many values that is, or -1 with a load error message on top. H's values
// argFirst .. argFirst + argCount - 1 are copied in as further arguments.
//
// Throws Failure (with nothing left on either stack) for misuse: a closed VM,
// or arguments that can't be copied.
template <typename Setup>
Outcome enterGuest(lua_State* H, Sandbox& sb, Setup&& setup, int argFirst, int argCount) {
    flush(H, sb);
    if (sb.G == nullptr) {
        throw Failure("the VM is closed");
    }

    // A proxy collected while VM code runs may drop the last other reference.
    const std::shared_ptr<Sandbox> keepAlive = sb.self.lock();
    lua_State* G = sb.G;
    const int guestTop = lua_gettop(G);

    if (!lua_checkstack(G, 2)) {
        throw Failure("VM stack overflow");
    }
    lua_State* T = lua_newthread(G); // anchored on the guest's main stack

    Outcome outcome;
    if (!lua_checkstack(T, argCount + 8)) {
        lua_settop(G, guestTop);
        throw Failure("too many arguments for the VM");
    }

    const int fixed = setup(T);
    if (fixed < 0) {
        pushErrorValue(sb, T, H, false);
        lua_settop(G, guestTop);
        outcome.kind = Kind::Syntax;
        return outcome;
    }

    {
        Copier arguments(sb, H, T, true);
        if (!arguments.copyRange(argFirst, argCount)) {
            std::string message = arguments.error();
            lua_settop(G, guestTop);
            throw Failure(message);
        }
    }

    lua_State* const previousHost = sb.host;
    const bool previousEnforcing = sb.alloc.enforcing;

    if (sb.depth == 0) {
        // Luau has no emergency collection: garbage left by an earlier call
        // (one that ran out of memory, say) would make this one fail too.
        if (sb.alloc.limit != 0 && sb.alloc.used > sb.alloc.limit) {
            lua_gc(G, LUA_GCCOLLECT, 0);
        }
        sb.timedOut = false;
        sb.alloc.hitLimit = false;
        sb.ticks = 0;
        sb.budget = sb.options.timeout;
        sb.deadline = sb.budget > 0 ? lua_clock() + sb.budget : 0;
    }
    sb.host = H;
    ++sb.depth;
    ++sb.calls;
    sb.alloc.enforcing = true;

    const int status = lua_resume(T, H, fixed - 1 + argCount); // the function itself isn't an argument

    sb.alloc.enforcing = previousEnforcing;
    --sb.depth;
    sb.host = previousHost;
    if (sb.depth == 0) {
        sb.deadline = 0;
    }

    if (status == LUA_OK) {
        const int n = lua_gettop(T);
        Copier results(sb, T, H, false);
        if (!lua_checkstack(H, n + 2)) {
            lua_pushstring(H, "too many results from the VM");
            outcome.kind = Kind::Runtime;
        } else if (!results.copyRange(1, n)) {
            lua_pushstring(H, results.error().c_str());
            outcome.kind = Kind::Runtime;
        } else {
            outcome.results = n;
        }
    } else if (status == LUA_YIELD) {
        lua_pushstring(H, "VM code yielded outside of a coroutine");
        outcome.kind = Kind::Yield;
    } else {
        if (const char* trace = lua_debugtrace(T)) {
            outcome.traceback = trace;
        }

        if (sb.timedOut) {
            outcome.kind = Kind::Timeout;
            ++sb.timeouts;
            pushErrorValue(sb, T, H, false);
        } else if (status == LUA_ERRMEM || (sb.alloc.hitLimit && isOutOfMemoryMessage(T, -1))) {
            outcome.kind = Kind::Memory;
            if (sb.alloc.hitLimit) {
                lua_pushfstring(H, "VM memory limit exceeded (%.0f bytes)", double(sb.alloc.limit));
            } else {
                pushErrorValue(sb, T, H, false);
            }
        } else {
            outcome.kind = Kind::Runtime;
            pushErrorValue(sb, T, H, false);
        }
    }

    if (outcome.kind != Kind::Ok) {
        ++sb.errors;
    }

    lua_settop(G, guestTop);
    if (outcome.kind == Kind::Memory && sb.depth == 0) {
        lua_gc(G, LUA_GCCOLLECT, 0); // what the failed call left behind
    }
    return outcome;
}

// A guest function, as the host sees it.
int callGuest(lua_State* L) {
    const auto* proxy = static_cast<const GuestProxy*>(lua_touserdata(L, lua_upvalueindex(1)));

    return guarded(L, [&] {
        const std::shared_ptr<Sandbox> sb = proxy->sandbox;
        if (sb->G == nullptr) {
            throw Failure("this function belongs to a VM that has been closed");
        }
        if (sb->generation != proxy->generation) {
            throw Failure("this function belongs to a VM that has been reset since");
        }

        const int ref = proxy->ref;
        const Outcome outcome = enterGuest(
            L, *sb,
            [ref](lua_State* T) {
                lua_getref(T, ref);
                return 1;
            },
            1, lua_gettop(L));
        return outcome.kind == Kind::Ok ? outcome.results : -1;
    });
}

// inside the guest //////////////////////////////////////////////////////////
// These run on guest threads, inside the guest's protection: raising is fine.

// Walks "path" ("a.b.c") from the VM's globals and leaves [container, key]
// for the last segment on top of the stack. A missing or non-table step
// raises, unless "lenient", which pushes [nil, nil] instead.
bool walkPath(lua_State* L, int pathIndex, bool lenient) {
    std::size_t length = 0;
    const char* path = luaL_checklstring(L, pathIndex, &length);

    lua_getref(L, sandboxOf(L).envRef);
    std::size_t start = 0;

    while (true) {
        std::size_t dot = start;
        while (dot < length && path[dot] != '.') {
            ++dot;
        }
        if (dot == start) {
            luaL_error(L, "invalid path '%s'", path);
        }

        lua_pushlstring(L, path + start, dot - start);
        if (dot == length) {
            return true;
        }

        lua_gettable(L, -2);
        if (!lua_istable(L, -1)) {
            if (lenient) {
                lua_pop(L, 2);
                lua_pushnil(L);
                lua_pushnil(L);
                return false;
            }
            luaL_error(L, "'%.*s' is not a table", static_cast<int>(dot), path);
        }
        lua_remove(L, -2);
        start = dot + 1;
    }
}

int guestGet(lua_State* L) {
    if (!walkPath(L, 1, true)) {
        lua_pushnil(L);
        return 1;
    }
    lua_gettable(L, -2);
    return 1;
}

int guestSet(lua_State* L) {
    walkPath(L, 1, false);
    lua_pushvalue(L, 2);
    lua_settable(L, -3);
    return 0;
}

int guestCall(lua_State* L) {
    const int nargs = lua_gettop(L) - 1;
    walkPath(L, 1, false);
    lua_gettable(L, -2);
    if (lua_isnil(L, -1)) {
        luaL_error(L, "the VM has no function '%s'", lua_tostring(L, 1));
    }

    lua_replace(L, 1); // [function, args..., container]
    lua_pop(L, 1);
    lua_call(L, nargs, LUA_MULTRET);
    return lua_gettop(L);
}

int guestRequire(lua_State* L) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    lua_settop(L, 1);

    lua_rawgetfield(L, LUA_REGISTRYINDEX, kModulesKey); // 2: cache
    lua_pushvalue(L, 1);
    lua_rawget(L, 2);
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
        luaL_error(L, "cyclic require of module '%s'", name);
    }
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);

    Sandbox& sb = sandboxOf(L);
    const auto found = sb.options.modules.find(std::string_view(name, length));
    if (found == sb.options.modules.end()) {
        luaL_error(L, "module '%s' not found (add it with the 'modules' option or vm:defineModule)", name);
    }
    const Module& module = found->second;

    lua_pushvalue(L, 1);
    lua_pushlightuserdata(L, &sb); // "loading" marker
    lua_rawset(L, 2);

    lua_getref(L, sb.envRef);
    const int env = lua_gettop(L);
    const int loaded = luau_load(L, module.chunkName.c_str(), module.bytecode.data(), module.bytecode.size(), env);
    lua_remove(L, env);

    if (loaded != 0 || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        lua_pushvalue(L, 1); // clear the marker, so a later require tries again
        lua_pushnil(L);
        lua_rawset(L, 2);
        lua_error(L);
    }

    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, -2);
    lua_rawset(L, 2);
    return 1;
}

// print = function: the line goes to the host function.
int guestPrint(lua_State* L) {
    const int n = lua_gettop(L);
    luaL_Strbuf buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) {
            luaL_addchar(&buffer, '\t');
        }
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);

    Sandbox& sb = sandboxOf(L);
    if (invokeHost(L, sb, sb.printRef, lua_gettop(L), 1) < 0) {
        lua_error(L);
    }
    return 0;
}

// print = false
int guestPrintSilent(lua_State*) { return 0; }

// Fills a fresh guest VM. Runs protected, since anything may run out of memory.
int setupGuest(lua_State* L) {
    Sandbox& sb = *static_cast<Sandbox*>(lua_touserdata(L, 1));
    luaL_openlibs(L);

    if (!sb.options.allLibraries) {
        for (const char* library : kStandardLibraries) {
            const auto& wanted = sb.options.libraries;
            if (std::find(wanted.begin(), wanted.end(), library) == wanted.end()) {
                lua_pushnil(L);
                lua_setglobal(L, library);
            }
        }
    }

    if (sb.options.print != PrintMode::Default) {
        lua_pushcfunction(L, sb.options.print == PrintMode::Host ? guestPrint : guestPrintSilent, "print");
        lua_setglobal(L, "print");
    }

    lua_pushcfunction(L, guestRequire, "require");
    lua_setglobal(L, "require");

    lua_newtable(L);
    lua_rawsetfield(L, LUA_REGISTRYINDEX, kModulesKey);

    lua_newtable(L); // host ref -> proxy, weak so proxies can still be collected
    lua_newtable(L);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetfield(L, LUA_REGISTRYINDEX, kHostProxiesKey);

    if (sb.options.sandbox) {
        // Libraries and the base globals become read-only. Chunks run in one
        // shared, writable table on top of them, so globals persist between
        // calls. It is not marked "safeenv": chunks loaded at different times
        // share it, so it really can change under them.
        luaL_sandbox(L);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_setfield(L, -2, "__index");
        lua_setreadonly(L, -1, 1);
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "_G");
    } else {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
    }

    sb.envRef = lua_ref(L, -1);
    return 0;
}

// Copies options.globals into the VM's globals. Throws Failure.
void copyGlobals(lua_State* H, Sandbox& sb) {
    if (sb.globalsRef == LUA_NOREF) {
        return;
    }

    lua_State* G = sb.G;
    const int hostTop = lua_gettop(H);
    const int guestTop = lua_gettop(G);
    std::string error;

    lua_getref(H, sb.globalsRef);
    const int table = lua_gettop(H);
    lua_getref(G, sb.envRef);
    const int env = lua_gettop(G);

    lua_pushnil(H);
    while (error.empty() && lua_next(H, table) != 0) {
        if (lua_type(H, -2) != LUA_TSTRING) {
            error = "luau.vm: option 'globals' must only have string keys";
            break;
        }

        Copier copier(sb, H, G, true);
        if (!copier.copyRange(-2, 2)) {
            error = "luau.vm: globals." + stringAt(H, -2) + ": " + copier.error();
            break;
        }
        lua_rawset(G, env);
        lua_pop(H, 1);
    }

    lua_settop(H, hostTop);
    lua_settop(G, guestTop);
    if (!error.empty()) {
        throw Failure(error);
    }
}

// Creates the guest VM from sb.options. Throws Failure.
void openGuest(lua_State* H, Sandbox& sb) {
    sb.alloc.enforcing = false;
    lua_State* G = lua_newstate(allocate, &sb.alloc);
    if (G == nullptr) {
        throw Failure("luau.vm: not enough memory to create a VM");
    }

    sb.G = G;
    lua_callbacks(G)->userdata = &sb;

    lua_pushcfunction(G, setupGuest, "setup");
    lua_pushlightuserdata(G, &sb);
    if (lua_pcall(G, 1, 0, 0) != LUA_OK) {
        std::string message = "luau.vm: could not create the VM: " + stringAt(G, -1);
        closeGuest(sb);
        throw Failure(message);
    }

    try {
        copyGlobals(H, sb);
    } catch (const Failure&) {
        closeGuest(sb);
        throw;
    }

    applyTimeout(sb);
    sb.alloc.peak = sb.alloc.used;
}

// host side: the VM object ///////////////////////////////////////////////////

struct VMBox {
    std::shared_ptr<Sandbox> sandbox;
};

Sandbox& checkVM(lua_State* L) {
    auto* box = static_cast<VMBox*>(luaL_checkudata(L, 1, kVMMeta));
    return *box->sandbox;
}

// Throws Failure if the VM can't be stopped right now.
void checkNotRunning(const Sandbox& sb, const char* what) {
    if (sb.depth > 0) {
        throw Failure(std::string("cannot ") + what + " a VM while it is running code");
    }
}

// Reads the VM options at "index" into sb. Throws Failure.
void readVMOptions(lua_State* L, int index, Sandbox& sb) {
    const Options options(L, index, "luau.vm",
                          {"name", "libraries", "sandbox", "globals", "memoryLimit", "timeout", "print", "modules",
                           "optimizationLevel", "debugLevel"});
    VMOptions& o = sb.options;

    if (std::optional<std::string> name = options.string("name")) {
        if (name->empty()) {
            options.fail("name", "a non-empty string");
        }
        o.name = std::move(*name);
    }

    if (std::optional<std::vector<std::string>> libraries = options.strings("libraries")) {
        for (const std::string& library : *libraries) {
            if (libraryIndex(library) < 0) {
                throw Failure("luau.vm: unknown library '" + library + "'");
            }
        }
        o.libraries = std::move(*libraries);
        o.allLibraries = false;
    }

    o.sandbox = options.boolean("sandbox").value_or(true);
    o.levels = readLevels(options);

    if (const std::optional<double> limit = options.number("memoryLimit")) {
        if (!(*limit >= 0) || *limit > 1e15) {
            options.fail("memoryLimit", "a number of bytes (0 for none)");
        }
        sb.alloc.limit = static_cast<std::size_t>(*limit);
    }

    if (const std::optional<double> timeout = options.number("timeout")) {
        if (!(*timeout >= 0) || *timeout > 1e9) {
            options.fail("timeout", "a number of seconds (0 for none)");
        }
        o.timeout = *timeout;
    }

    switch (options.type("print")) {
    case LUA_TNIL:
        break;
    case LUA_TFUNCTION:
        o.print = PrintMode::Host;
        sb.printRef = lua_ref(L, options.push("print"));
        lua_pop(L, 1);
        break;
    case LUA_TBOOLEAN:
        if (options.boolean("print").value_or(true)) {
            options.fail("print", "a function or false");
        }
        o.print = PrintMode::Silent;
        break;
    default:
        options.fail("print", "a function or false");
    }

    switch (options.type("globals")) {
    case LUA_TNIL:
        break;
    case LUA_TTABLE:
        sb.globalsRef = lua_ref(L, options.push("globals"));
        lua_pop(L, 1);
        break;
    default:
        options.fail("globals", "a table");
    }

    switch (options.type("modules")) {
    case LUA_TNIL:
        break;
    case LUA_TTABLE: {
        const int modules = options.push("modules");
        lua_pushnil(L);
        while (lua_next(L, modules) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 3);
                options.fail("modules", "a table of module names to source code");
            }

            const std::string name = stringAt(L, -2);
            Module module{chunkName(name), {}};
            std::string error;
            const Chunk chunk{lua_tostring(L, -1), static_cast<std::size_t>(lua_objlen(L, -1)), false};
            if (!toBytecode(chunk, o.levels, name, module.bytecode, error)) {
                lua_pop(L, 3);
                throw Failure("luau.vm: module '" + name + "': " + error);
            }
            o.modules.insert_or_assign(name, std::move(module));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        break;
    }
    default:
        options.fail("modules", "a table");
    }
}

// luau.vm(options?): VM
int luauVM(lua_State* L) {
    flushGraveyard(L);

    return guarded(L, [&] {
        auto sb = std::make_shared<Sandbox>();
        sb->self = sb;
        if (std::shared_ptr<Graveyard>* box = graveyardBox(L)) {
            sb->graveyard = *box;
        }

        readVMOptions(L, 1, *sb);
        openGuest(L, *sb);

        void* memory = lua_newuserdatadtor(L, sizeof(VMBox), [](lua_State*, void* p) { static_cast<VMBox*>(p)->~VMBox(); });
        new (memory) VMBox{std::move(sb)};
        luaL_getmetatable(L, kVMMeta);
        lua_setmetatable(L, -2);
        return 1;
    });
}

// Compiles a chunk argument for the VM. False with the message pushed onto L
// for a syntax error.
bool compileForVM(lua_State* L, const Sandbox& sb, const Chunk& chunk, const std::string& name, std::string& bytecode) {
    std::string error;
    if (toBytecode(chunk, sb.options.levels, name, bytecode, error)) {
        return true;
    }
    lua_pushlstring(L, error.data(), error.size());
    return false;
}

// Runs a chunk argument (at index 2) with the values after it as "...".
Outcome runChunk(lua_State* L, Sandbox& sb, const Chunk& chunk) {
    const int argCount = lua_gettop(L) - 2;

    std::string bytecode;
    if (!compileForVM(L, sb, chunk, sb.options.name, bytecode)) {
        Outcome outcome;
        outcome.kind = Kind::Syntax;
        return outcome;
    }

    const std::string name = chunkName(sb.options.name);
    const int envRef = sb.envRef;
    return enterGuest(
        L, sb,
        [&](lua_State* T) {
            lua_getref(T, envRef);
            const int env = lua_gettop(T);
            const int status = luau_load(T, name.c_str(), bytecode.data(), bytecode.size(), env);
            lua_remove(T, env);
            return status == 0 ? 1 : -1;
        },
        3, argCount);
}

// vm:run(chunk, ...): ...
int vmRun(lua_State* L) {
    Sandbox& sb = checkVM(L);
    const Chunk chunk = checkChunk(L, 2);

    return guarded(L, [&] {
        const Outcome outcome = runChunk(L, sb, chunk);
        return outcome.kind == Kind::Ok ? outcome.results : -1;
    });
}

// vm:try(chunk, ...): (true, ...) | (false, VMError)
int vmTry(lua_State* L) {
    Sandbox& sb = checkVM(L);
    const Chunk chunk = checkChunk(L, 2);

    return guarded(L, [&] {
        const Outcome outcome = runChunk(L, sb, chunk);

        if (outcome.kind == Kind::Ok) {
            lua_pushboolean(L, 1);
            lua_insert(L, -(outcome.results + 1));
            return outcome.results + 1;
        }

        const int value = lua_gettop(L);
        lua_pushboolean(L, 0);
        lua_createtable(L, 0, 4);
        lua_pushstring(L, kindName(outcome.kind));
        lua_setfield(L, -2, "kind");
        if (lua_type(L, value) == LUA_TSTRING) {
            lua_pushvalue(L, value);
        } else {
            lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, value));
        }
        lua_setfield(L, -2, "message");
        lua_pushvalue(L, value);
        lua_setfield(L, -2, "value");
        lua_pushlstring(L, outcome.traceback.data(), outcome.traceback.size());
        lua_setfield(L, -2, "traceback");
        return 2;
    });
}

// vm:load(chunk, name?): function
int vmLoad(lua_State* L) {
    Sandbox& sb = checkVM(L);
    const Chunk chunk = checkChunk(L, 2);
    std::size_t nameLength = 0;
    const char* nameArg = luaL_optlstring(L, 3, nullptr, &nameLength);

    return guarded(L, [&] {
        flush(L, sb);
        if (sb.G == nullptr) {
            throw Failure("the VM is closed");
        }

        const std::string name = nameArg ? std::string(nameArg, nameLength) : sb.options.name;
        std::string bytecode;
        if (!compileForVM(L, sb, chunk, name, bytecode)) {
            return -1;
        }

        lua_State* G = sb.G;
        const int guestTop = lua_gettop(G);
        lua_getref(G, sb.envRef);
        if (luau_load(G, chunkName(name).c_str(), bytecode.data(), bytecode.size(), guestTop + 1) != 0) {
            pushErrorValue(sb, G, L, false);
            lua_settop(G, guestTop);
            return -1;
        }

        Copier copier(sb, G, L, false);
        const bool copied = copier.copyRange(-1, 1);
        lua_settop(G, guestTop);
        if (!copied) {
            throw Failure(copier.error());
        }
        return 1;
    });
}

// vm:call(path, ...) / vm:get(path) / vm:set(path, value): through a guest
// trampoline, so metamethods, limits and errors behave as in vm:run.
int pathCall(lua_State* L, lua_CFunction trampoline, const char* name, int argCount) {
    Sandbox& sb = checkVM(L);
    luaL_checkstring(L, 2);

    return guarded(L, [&] {
        const Outcome outcome = enterGuest(
            L, sb,
            [&](lua_State* T) {
                lua_pushcfunction(T, trampoline, name);
                std::size_t length = 0;
                const char* path = lua_tolstring(L, 2, &length);
                lua_pushlstring(T, path, length);
                return 2;
            },
            3, argCount);
        return outcome.kind == Kind::Ok ? outcome.results : -1;
    });
}

int vmCall(lua_State* L) { return pathCall(L, guestCall, "call", std::max(lua_gettop(L) - 2, 0)); }
int vmGet(lua_State* L) { return pathCall(L, guestGet, "get", 0); }
int vmSet(lua_State* L) { return pathCall(L, guestSet, "set", 1); }

// vm:globals(): { string }
int vmGlobals(lua_State* L) {
    Sandbox& sb = checkVM(L);

    return guarded(L, [&] {
        flush(L, sb);
        if (sb.G == nullptr) {
            throw Failure("the VM is closed");
        }

        lua_State* G = sb.G;
        const int guestTop = lua_gettop(G);
        std::vector<std::string> names;

        lua_getref(G, sb.envRef);
        lua_pushvalue(G, LUA_GLOBALSINDEX);
        for (int table = guestTop + 1; table <= guestTop + 2; ++table) {
            lua_pushnil(G);
            while (lua_next(G, table) != 0) {
                if (lua_type(G, -2) == LUA_TSTRING) {
                    names.push_back(stringAt(G, -2));
                }
                lua_pop(G, 1);
            }
        }
        lua_settop(G, guestTop);

        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());

        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (std::size_t i = 0; i < names.size(); ++i) {
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        return 1;
    });
}

// vm:defineModule(name, source)
int vmDefineModule(lua_State* L) {
    Sandbox& sb = checkVM(L);
    std::size_t nameLength = 0;
    const char* nameArg = luaL_checklstring(L, 2, &nameLength);
    const Chunk chunk = checkChunk(L, 3);

    return guarded(L, [&] {
        const std::string name(nameArg, nameLength);
        Module module{chunkName(name), {}};
        if (!compileForVM(L, sb, chunk, name, module.bytecode)) {
            return -1;
        }
        sb.options.modules.insert_or_assign(name, std::move(module));
        return 0;
    });
}

// vm:setTimeout(seconds?)
int vmSetTimeout(lua_State* L) {
    Sandbox& sb = checkVM(L);
    const double seconds = luaL_optnumber(L, 2, 0);
    luaL_argcheck(L, seconds >= 0 && seconds <= 1e9, 2, "timeout must be a number of seconds (0 for none)");

    sb.options.timeout = seconds;
    applyTimeout(sb);
    return 0;
}

// vm:setMemoryLimit(bytes?)
int vmSetMemoryLimit(lua_State* L) {
    Sandbox& sb = checkVM(L);
    const double bytes = luaL_optnumber(L, 2, 0);
    luaL_argcheck(L, bytes >= 0 && bytes <= 1e15, 2, "limit must be a number of bytes (0 for none)");

    sb.alloc.limit = static_cast<std::size_t>(bytes);
    return 0;
}

// vm:stats(): table
int vmStats(lua_State* L) {
    const Sandbox& sb = checkVM(L);

    lua_createtable(L, 0, 7);
    lua_pushnumber(L, double(sb.alloc.used));
    lua_setfield(L, -2, "memory");
    lua_pushnumber(L, double(sb.alloc.peak));
    lua_setfield(L, -2, "peakMemory");
    if (sb.alloc.limit != 0) {
        lua_pushnumber(L, double(sb.alloc.limit));
        lua_setfield(L, -2, "memoryLimit");
    }
    if (sb.options.timeout > 0) {
        lua_pushnumber(L, sb.options.timeout);
        lua_setfield(L, -2, "timeout");
    }
    lua_pushnumber(L, double(sb.calls));
    lua_setfield(L, -2, "calls");
    lua_pushnumber(L, double(sb.errors));
    lua_setfield(L, -2, "errors");
    lua_pushnumber(L, double(sb.timeouts));
    lua_setfield(L, -2, "timeouts");
    return 1;
}

// vm:collect(): number
int vmCollect(lua_State* L) {
    Sandbox& sb = checkVM(L);

    return guarded(L, [&] {
        if (sb.G == nullptr) {
            throw Failure("the VM is closed");
        }
        lua_gc(sb.G, LUA_GCCOLLECT, 0);
        flush(L, sb);
        lua_pushnumber(L, double(sb.alloc.used));
        return 1;
    });
}

// vm:reset(): a fresh VM with the same options
int vmReset(lua_State* L) {
    Sandbox& sb = checkVM(L);

    return guarded(L, [&] {
        checkNotRunning(sb, "reset");
        flush(L, sb);
        closeGuest(sb);
        releaseHostRefs(sb, L, false);
        sb.calls = sb.errors = sb.timeouts = 0;
        openGuest(L, sb);
        return 0;
    });
}

// vm:close()
int vmClose(lua_State* L) {
    Sandbox& sb = checkVM(L);

    return guarded(L, [&] {
        checkNotRunning(sb, "close");
        flush(L, sb);
        closeGuest(sb);
        releaseHostRefs(sb, L, true);
        return 0;
    });
}

// vm:isClosed(): boolean
int vmIsClosed(lua_State* L) {
    lua_pushboolean(L, checkVM(L).G == nullptr);
    return 1;
}

int vmToString(lua_State* L) {
    const Sandbox& sb = checkVM(L);
    lua_pushfstring(L, "LuauVM(%s%s)", sb.options.name.c_str(), sb.G == nullptr ? ", closed" : "");
    return 1;
}

// in this VM ////////////////////////////////////////////////////////////////

// luau.compile(source, options?): buffer
int luauCompile(lua_State* L) {
    std::size_t length = 0;
    const char* source = luaL_checklstring(L, 1, &length);

    return guarded(L, [&] {
        const Options options(L, 2, "luau.compile", {"name", "optimizationLevel", "debugLevel"});
        const std::string name = options.string("name").value_or(kDefaultChunkName);

        std::string bytecode;
        std::string error;
        if (!toBytecode({source, length, false}, readLevels(options), name, bytecode, error)) {
            throw Failure(error);
        }

        void* target = lua_newbuffer(L, bytecode.size());
        std::memcpy(target, bytecode.data(), bytecode.size());
        return 1;
    });
}

// luau.check(source, name?): (boolean, string?)
int luauCheck(lua_State* L) {
    std::size_t length = 0;
    const char* source = luaL_checklstring(L, 1, &length);
    std::size_t nameLength = 0;
    const char* nameArg = luaL_optlstring(L, 2, kDefaultChunkName, &nameLength);

    return guarded(L, [&] {
        std::string bytecode;
        std::string error;
        const bool ok = toBytecode({source, length, false}, {0, 0}, std::string(nameArg, nameLength), bytecode, error);

        lua_pushboolean(L, ok);
        if (ok) {
            return 1;
        }
        lua_pushlstring(L, error.data(), error.size());
        return 2;
    });
}

// luau.load(chunk, options?): function | (nil, string)
int luauLoad(lua_State* L) {
    const Chunk chunk = checkChunk(L, 1);

    return guarded(L, [&] {
        const Options options(L, 2, "luau.load", {"name", "env", "optimizationLevel", "debugLevel"});
        const std::string name = options.string("name").value_or(kDefaultChunkName);
        const Levels levels = readLevels(options);

        int env = options.push("env");
        if (lua_isnil(L, env)) {
            // A fresh global table that reads through to the caller's, so the
            // chunk sees everything but keeps its own globals to itself.
            lua_pop(L, 1);
            lua_newtable(L);
            lua_newtable(L);
            lua_pushvalue(L, LUA_GLOBALSINDEX);
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, -2);
            env = lua_gettop(L);
        } else if (!lua_istable(L, env)) {
            options.fail("env", "a table");
        }

        std::string bytecode;
        std::string error;
        if (!toBytecode(chunk, levels, name, bytecode, error)) {
            lua_pushnil(L);
            lua_pushlstring(L, error.data(), error.size());
            return 2;
        }

        if (luau_load(L, chunkName(name).c_str(), bytecode.data(), bytecode.size(), env) != 0) {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        return 1;
    });
}

// luau.env(options?): table
int luauEnv(lua_State* L) {
    bool include[std::size(kStandardLibraries)] = {};
    for (std::size_t i = 0; i < std::size(kStandardLibraries); ++i) {
        include[i] = std::strcmp(kStandardLibraries[i], "debug") != 0; // debug can reach other functions' locals
    }

    int globals = 0;
    guarded(L, [&] {
        const Options options(L, 1, "luau.env", {"libraries", "globals"});

        if (std::optional<std::vector<std::string>> libraries = options.strings("libraries")) {
            std::fill(std::begin(include), std::end(include), false);
            for (const std::string& library : *libraries) {
                const int index = libraryIndex(library);
                if (index < 0) {
                    throw Failure("luau.env: unknown library '" + library + "'");
                }
                include[index] = true;
            }
        }

        const int type = options.type("globals");
        if (type == LUA_TTABLE) {
            globals = options.push("globals");
        } else if (type != LUA_TNIL) {
            options.fail("globals", "a table");
        }
        return 0;
    });

    lua_createtable(L, 0, static_cast<int>(std::size(kSafeBaseFunctions) + std::size(kStandardLibraries)));
    const int env = lua_gettop(L);

    for (const char* name : kSafeBaseFunctions) {
        lua_getfield(L, LUA_GLOBALSINDEX, name);
        lua_rawsetfield(L, env, name);
    }
    for (std::size_t i = 0; i < std::size(kStandardLibraries); ++i) {
        if (include[i]) {
            lua_getfield(L, LUA_GLOBALSINDEX, kStandardLibraries[i]);
            lua_rawsetfield(L, env, kStandardLibraries[i]);
        }
    }
    lua_pushvalue(L, env);
    lua_rawsetfield(L, env, "_G");

    if (globals != 0) {
        lua_pushnil(L);
        while (lua_next(L, globals) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                luaL_error(L, "luau.env: option 'globals' must only have string keys");
            }
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, env);
        }
    }

    lua_pushvalue(L, env);
    return 1;
}

// luau.eval is written in Luau, so that the evaluated code may yield.
constexpr const char* kEvalSource = R"lua(
local load, error, typeof = ...
return function(source, env, ...)
    if typeof(source) ~= "string" then
        error("invalid argument #1 to 'eval' (string expected, got " .. typeof(source) .. ")", 2)
    end
    if env ~= nil and typeof(env) ~= "table" then
        error("invalid argument #2 to 'eval' (table expected, got " .. typeof(env) .. ")", 2)
    end
    local fn, message = load(source, { name = "eval", env = env })
    if not fn then
        error(message, 0)
    end
    return fn(...)
end
)lua";

void pushEval(lua_State* L) {
    static const std::string bytecode = [] {
        std::string result;
        std::string error;
        toBytecode({kEvalSource, std::strlen(kEvalSource), false}, {}, "@sonata/luau", result, error);
        return result;
    }();

    if (luau_load(L, "=@sonata/luau", bytecode.data(), bytecode.size(), 0) != 0) {
        lua_error(L);
    }
    lua_pushcfunction(L, luauLoad, "load");
    lua_getglobal(L, "error");
    lua_getglobal(L, "typeof");
    lua_call(L, 3, 1);
}

constexpr NativeFunction kFunctions[] = {
    {"compile", luauCompile},
    {"check", luauCheck},
    {"load", luauLoad},
    {"env", luauEnv},
    {"vm", luauVM},
};

constexpr NativeFunction kVMMethods[] = {
    {"run", vmRun},
    {"try", vmTry},
    {"load", vmLoad},
    {"call", vmCall},
    {"get", vmGet},
    {"set", vmSet},
    {"globals", vmGlobals},
    {"defineModule", vmDefineModule},
    {"setTimeout", vmSetTimeout},
    {"setMemoryLimit", vmSetMemoryLimit},
    {"stats", vmStats},
    {"collect", vmCollect},
    {"reset", vmReset},
    {"close", vmClose},
    {"isClosed", vmIsClosed},
};

} // namespace

// require("@sonata/luau")
void openLuau(lua_State* L) {
    lua_rawgetfield(L, LUA_REGISTRYINDEX, kGraveyardKey);
    if (lua_isnil(L, -1)) {
        void* memory = lua_newuserdatadtor(L, sizeof(std::shared_ptr<Graveyard>), [](lua_State*, void* p) {
            static_cast<std::shared_ptr<Graveyard>*>(p)->~shared_ptr();
        });
        new (memory) std::shared_ptr<Graveyard>(std::make_shared<Graveyard>());
        lua_rawsetfield(L, LUA_REGISTRYINDEX, kGraveyardKey);
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, kVMMeta)) {
        lua_createtable(L, 0, static_cast<int>(std::size(kVMMethods)));
        setFunctions(L, kVMMethods);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, vmToString, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "LuauVM");
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 3);
    setFunctions(L, kFunctions);

    pushEval(L);
    lua_setfield(L, -2, "eval");

    lua_createtable(L, static_cast<int>(std::size(kStandardLibraries)), 0);
    int count = 0;
    for (const char* library : kStandardLibraries) {
        lua_getglobal(L, library);
        const bool present = lua_istable(L, -1);
        lua_pop(L, 1);
        if (present) {
            lua_pushstring(L, library);
            lua_rawseti(L, -2, ++count);
        }
    }
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "libraries");

    lua_pushlstring(L, info::luau_version.data(), info::luau_version.size());
    lua_setfield(L, -2, "version");
}

} // namespace sonata::lib::libs
