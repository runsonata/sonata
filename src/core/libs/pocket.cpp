// @sonata/pocket: read-only files packed with the program (build.assets).
//
//     local pocket = require("@sonata/pocket")
//
//     pocket.read(path: string): string           raises an error if there is no such asset
//     pocket.tryRead(path: string): string?        nil if there is no such asset
//     pocket.exists(path: string): boolean
//     pocket.list(folder: string?): { string }     every asset (under "folder", if given), sorted
//     pocket.bundled: boolean                      true inside a program made by "sn build"
//
// Paths are relative to the root of the package the calling script belongs
// to, use '/', and may not leave it ("assets/logo.png", "./config.json").
// Only files listed in that package's build.assets are visible, under
// "sn run" as well as in a built program. See sonata/core/pocket.hpp.

#include <cstring>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <sonata/core/library.hpp>
#include <sonata/core/pocket.hpp>
#include "lua.h"
#include "lualib.h"

namespace sonata::lib::libs {

namespace {

// The module path of the Luau code that called us ("" if unknown). Same walk
// as require's: skip C frames, take the first chunk the loader named.
std::string callerModule(lua_State* L) {
    lua_Debug ar;

    for (int level = 1; lua_getinfo(L, level, "s", &ar); ++level) {
        if (std::strcmp(ar.what, "C") == 0) {
            continue;
        }

        if (ar.source != nullptr && ar.source[0] == '@') {
            return std::string(ar.source + 1);
        }

        break;
    }

    return {};
}

// Everything a call needs, resolved before any Lua error can be raised.
struct Call {
    const Pocket* pocket = nullptr;
    std::string package;
    std::optional<std::string> path; // normalised; nullopt if invalid
    std::string error;               // set if the call can't proceed
};

Call prepare(lua_State* L, const char* rawPath) {
    Call call;
    call.pocket = Pocket::of(L);

    if (call.pocket == nullptr) {
        call.error = "pocket: no assets are available in this environment";
        return call;
    }

    const std::optional<std::string> package = call.pocket->packageOf(callerModule(L));
    if (!package) {
        call.error = "pocket: the calling script doesn't belong to a package";
        return call;
    }
    call.package = *package;

    if (rawPath != nullptr) {
        call.path = Pocket::normalize(rawPath);
        if (!call.path) {
            call.error = std::string("pocket: '") + rawPath + "' is not a path inside the package";
        }
    }

    return call;
}

// Reads the asset into "out". Returns false with "error" set on failure;
// a missing asset is not a failure (out stays nullopt).
bool fetch(const Call& call, std::optional<std::string>& out, std::string& error) {
    try {
        out = call.pocket->read(call.package, *call.path);
        return true;
    } catch (const std::exception& e) {
        error = std::string("pocket: ") + e.what();
        return false;
    }
}

void pushString(lua_State* L, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size());
}

// The C++ objects live in this helper's frame, which has returned before
// lua_error() runs in the caller.
bool readInto(lua_State* L, bool optional, std::string& error) {
    const char* path = luaL_checkstring(L, 1);

    const Call call = prepare(L, path);
    if (!call.error.empty()) {
        error = call.error;
        return false;
    }

    std::optional<std::string> data;
    if (!fetch(call, data, error)) {
        return false;
    }

    if (!data) {
        if (optional) {
            lua_pushnil(L);
            return true;
        }
        error = "pocket: no asset '" + *call.path +
                "' in this package (list it in build.assets in .sonata/project.luau)";
        return false;
    }

    pushString(L, *data);
    return true;
}

int raise(lua_State* L, const std::string& message) {
    lua_pushlstring(L, message.data(), message.size());
    return -1;
}

int pocketRead(lua_State* L) {
    int results = 1;
    {
        std::string error;
        if (!readInto(L, false, error)) {
            results = raise(L, error);
        }
    }
    if (results < 0) {
        lua_error(L);
    }
    return results;
}

int pocketTryRead(lua_State* L) {
    int results = 1;
    {
        std::string error;
        if (!readInto(L, true, error)) {
            results = raise(L, error);
        }
    }
    if (results < 0) {
        lua_error(L);
    }
    return results;
}

bool existsInto(lua_State* L, std::string& error) {
    const char* path = luaL_checkstring(L, 1);

    const Call call = prepare(L, path);
    if (!call.error.empty()) {
        // An invalid path simply doesn't exist; a missing pocket is still an error.
        if (call.pocket != nullptr && !call.path) {
            lua_pushboolean(L, false);
            return true;
        }
        error = call.error;
        return false;
    }

    std::optional<std::string> data;
    if (!fetch(call, data, error)) {
        return false;
    }

    lua_pushboolean(L, data.has_value());
    return true;
}

int pocketExists(lua_State* L) {
    int results = 1;
    {
        std::string error;
        if (!existsInto(L, error)) {
            results = raise(L, error);
        }
    }
    if (results < 0) {
        lua_error(L);
    }
    return results;
}

bool listInto(lua_State* L, std::string& error) {
    const char* folder = luaL_optstring(L, 1, nullptr);

    const Call call = prepare(L, folder);
    if (!call.error.empty()) {
        error = call.error;
        return false;
    }

    std::vector<std::string> assets;
    try {
        assets = call.pocket->list(call.package);
    } catch (const std::exception& e) {
        error = std::string("pocket: ") + e.what();
        return false;
    }

    const std::string prefix = call.path ? *call.path + "/" : std::string();

    lua_createtable(L, static_cast<int>(assets.size()), 0);
    int index = 0;
    for (const std::string& asset : assets) {
        if (prefix.empty() || asset.compare(0, prefix.size(), prefix) == 0) {
            pushString(L, asset);
            lua_rawseti(L, -2, ++index);
        }
    }
    return true;
}

int pocketList(lua_State* L) {
    int results = 1;
    {
        std::string error;
        if (!listInto(L, error)) {
            results = raise(L, error);
        }
    }
    if (results < 0) {
        lua_error(L);
    }
    return results;
}

constexpr NativeFunction kFunctions[] = {
    {"read", pocketRead},
    {"tryRead", pocketTryRead},
    {"exists", pocketExists},
    {"list", pocketList},
};

} // namespace

// require("@sonata/pocket")
void openPocket(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 1);
    setFunctions(L, kFunctions);

    const Pocket* pocket = Pocket::of(L);
    lua_pushboolean(L, pocket != nullptr && pocket->bundled());
    lua_setfield(L, -2, "bundled");
}

} // namespace sonata::lib::libs
