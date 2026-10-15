#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace sonata::lib {

// Built-in libraries
// ------------------
// A built-in library is a Luau module implemented in C++ and shipped inside
// sonata itself. Scripts import it through the reserved "@sonata" alias:
//
//     local path = require("@sonata/path")
//
// They need no files on disk and no entry in project.luau.
//
// To write one:
//   1. Write the functions as plain lua_CFunctions.
//   2. Write an "opener" (libs::open<Name>) that pushes the module's value
//      (normally a table), in src/core/libs/<name>.cpp.
//   3. Add <name> to SONATA_ALL_LIBS in src/CMakeLists.txt; the registry is
//      generated from that list.
//   4. Describe its API in types/libs/<name>.luau, the types "sn init --lsp"
//      gives the Luau Language Server (tests/test_lsp.cpp checks it exists).
//
// See src/core/libs/path.cpp for a complete example.

// A function exposed to Luau. "name" is both its key in the library table and
// the name shown in error messages and stack traces.
//
// "continuation" is only needed by functions that suspend the calling thread
// through the scheduler and want to run again when it resumes (see
// sonata/core/scheduler.hpp). It is the lua_Continuation Luau calls instead of
// returning straight to the script.
struct NativeFunction {
    const char* name;
    int (*function)(lua_State* L);
    int (*continuation)(lua_State* L, int status) = nullptr;
};

// Called the first time a script requires the library.
//
// It must push exactly one value on the stack, which becomes the value of
// require(...). The loader caches that value, so the opener runs once per VM.
// If the value is a table, the loader marks it read-only (nested tables are up
// to the opener).
//
// Raising a Lua error (luaL_error) from an opener is fine: the require fails
// with that message. Don't let C++ exceptions escape it.
using NativeOpener = void (*)(lua_State* L);

struct NativeLibrary {
    // "path" makes the library available as require("@sonata/path").
    // Lowercase letters, digits, '_' and '-' only.
    std::string name;
    NativeOpener open = nullptr;
};

// Sets every function in "functions" as a field of the table on top of the stack.
void setFunctions(lua_State* L, const NativeFunction* functions, std::size_t count);

template <std::size_t N>
void setFunctions(lua_State* L, const NativeFunction (&functions)[N]) {
    setFunctions(L, functions, N);
}

// The set of libraries a ModuleLoader can serve.
class LibraryRegistry {
public:
    // Throws std::invalid_argument for a bad name, a missing opener, or a name
    // that is already taken.
    void add(NativeLibrary library);

    // nullptr if there is no library called "name".
    const NativeLibrary* find(std::string_view name) const;

    // All registered names, sorted. Used for error messages.
    std::vector<std::string> names() const;

private:
    std::map<std::string, NativeLibrary, std::less<>> libraries_;
};

// Adds every library compiled into this binary. ModuleLoader calls this itself.
// Which libraries that is depends on SONATA_LIBS at build time (see
// src/CMakeLists.txt); a runtime built by "sn build" may have only a few.
void registerBuiltinLibraries(LibraryRegistry& registry);

// The names of every library Sonata knows about, sorted, whether or not this
// binary has them compiled in. Used to validate build.libraries.
const std::vector<std::string>& knownLibraryNames();

} // namespace sonata::lib