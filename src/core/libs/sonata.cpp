// @sonata/sonata: access to Sonata's own internals (build info, projects,
// packages, data files).
//
//     local sonata = require("@sonata/sonata")
//
//     sonata.info  (read-only)
//         { name, id, binary, tagline, version = "0.1.0-alpha.1",
//           semver = { major, minor, patch, pre }, gitHash, luauVersion,
//           buildType, debug: boolean, platform, compiler }
//
//   Types
//     Dependency = { name: string, url: string, ref: string? }
//     Package    = Dependency + { path: string, commit: string, fresh: boolean }
//     Project    = { root, entrypoint, sonataDir, projectFile, dependenciesDir: string,
//                    hasDependenciesDirectory: boolean, sources: { string },
//                    manifest: { name, version, description, entrypoint: string,
//                                authors: { string }, dependencies: { Dependency } } }
//
//   Projects (plain snapshots, nothing stays open)
//     sonata.openProject(dir: string?): Project          dir must be a project root
//     sonata.findProject(start: string?): Project        searches start, then its parents
//     sonata.validateProject(dir: string?): (boolean, string?)
//     sonata.validateDependency(dep: Dependency): (boolean, string?)
//
//   Packages (the project is always the optional LAST argument and is found
//   like findProject; each call also returns the manager's progress log)
//     sonata.install(project: string?)                 --> { Package }, { string }
//     sonata.update(name: string?, project: string?)   --> { Package }, { string }
//     sonata.add(dep: Dependency, project: string?)    --> Package, { string }
//     sonata.remove(name: string, project: string?)    --> { string }, { string }  (deleted folders)
//     sonata.prune(project: string?)                   --> { string }, { string }
//     sonata.installed(project: string?)               --> { Package }, { string }
//
// Failures raise a Lua error carrying the C++ exception's message (use pcall).
// Unspecified strings are "" rather than nil; paths use '/'. Package calls block
// while git runs. Paths are relative to the current directory; the default is ".".

#include <exception>
#include <filesystem>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sonata/core/info.hpp>
#include <sonata/core/library.hpp>
#include <sonata/core/package.hpp>
#include <sonata/core/project.hpp>
#include <sonata/core/luau/datafile.hpp>
#include "lua.h"
#include "lualib.h"


namespace sonata::lib::libs {

namespace {

using sonata::luau::DataFile;
using sonata::luau::DataValue;


// C++ -> Lua ///////////////////////////////////////////////////
// The push() overloads call each other, so declare them all up front.
void push(lua_State* L, std::string_view value);
void push(lua_State* L, const std::string& value);
void push(lua_State* L, const std::filesystem::path& value);
void push(lua_State* L, const Project::Dependency& dependency);
void push(lua_State* L, const InstalledPackage& package);
void push(lua_State* L, const Project& project);
template <typename T> void push(lua_State* L, const std::vector<T>& items);

void push(lua_State* L, std::string_view value) { lua_pushlstring(L, value.data(), value.size()); }
void push(lua_State* L, const std::string& value) { push(L, std::string_view(value)); }
void push(lua_State* L, const std::filesystem::path& value) { push(L, value.generic_string()); }

// table[key] = value, for the table on top of the stack.
template <typename T>
void field(lua_State* L, const char* key, const T& value) {
    push(L, value);
    lua_setfield(L, -2, key);
}

void flag(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

void number(lua_State* L, const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

template <typename T>
void push(lua_State* L, const std::vector<T>& items) {
    lua_createtable(L, static_cast<int>(items.size()), 0);

    int index = 0;
    for (const T& item : items) {
        push(L, item);
        lua_rawseti(L, -2, ++index);
    }
}

void push(lua_State* L, const Project::Dependency& dependency) {
    lua_createtable(L, 0, 6);
    field(L, "name", dependency.name);
    field(L, "url", dependency.url);
    field(L, "ref", dependency.ref);
}

// A Package is a Dependency with extra fields.
void push(lua_State* L, const InstalledPackage& package) {
    push(L, package.dependency);
    field(L, "path", package.path);
    field(L, "commit", package.commit);
    flag(L, "fresh", package.fresh);
}

void push(lua_State* L, const Project& project) {
    const auto& manifest = project.manifest();

    lua_createtable(L, 0, 8);
    field(L, "root", project.root());
    field(L, "entrypoint", project.entrypoint());
    field(L, "sonataDir", project.sonataDir());
    field(L, "projectFile", project.projectFile());
    field(L, "dependenciesDir", project.dependenciesDir());
    flag(L, "hasDependenciesDirectory", project.hasDependenciesDirectory());
    field(L, "sources", project.sourceFiles());

    lua_createtable(L, 0, 6);
    field(L, "name", manifest.name);
    field(L, "version", manifest.version);
    field(L, "description", manifest.description);
    field(L, "entrypoint", manifest.entrypoint);
    field(L, "authors", manifest.authors);
    field(L, "dependencies", manifest.dependencies);
    lua_setfield(L, -2, "manifest");
}

// Throws (std::runtime_error) if it nests too deeply for the Lua stack.
void pushData(lua_State* L, const DataValue& value) {
    if (!lua_checkstack(L, 4)) {
        throw std::runtime_error("data is nested too deeply");
    }

    switch (value.kind()) {
    case DataValue::Kind::Nil:
        lua_pushnil(L);
        break;
    case DataValue::Kind::Boolean:
        lua_pushboolean(L, value.asBoolean());
        break;
    case DataValue::Kind::Number:
        lua_pushnumber(L, value.asNumber());
        break;
    case DataValue::Kind::String:
        push(L, value.asString());
        break;
    case DataValue::Kind::Table: {
        lua_createtable(L, 0, 0);

        int position = 0;
        for (const auto& entry : value.asTable()) {
            if (!entry.key) { // positional item: {a, b, c}
                pushData(L, entry.value);
                lua_rawseti(L, -2, ++position);
                continue;
            }

            if (entry.key->kind == DataValue::Key::Kind::String) {
                push(L, entry.key->string);
            } else {
                lua_pushnumber(L, entry.key->number);
            }
            pushData(L, entry.value);
            lua_rawset(L, -3);
        }
        break;
    }
    }
}

// Lua -> C++ ///////////////////////////////

// Runs fn(), which does the C++ work and returns how many results it pushed.
// fn must create every C++ object itself, so the caller's frame holds only
// plain values and nothing is skipped when lua_error unwinds.
template <typename F>
int guarded(lua_State* L, F&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        lua_pushstring(L, e.what());
    } catch (...) {
        lua_pushstring(L, "unknown error");
    }

    lua_error(L);
}

// validate*() shape: true, or false plus the reason. check() returns the
// error message, or nullopt when everything is fine. Never raises a Lua error.
template <typename F>
int validation(lua_State* L, F&& check) {
    std::optional<std::string> error;
    try {
        error = check();
    } catch (const std::exception& e) {
        error = e.what();
    }

    lua_pushboolean(L, !error);
    if (!error) {
        return 1;
    }

    push(L, *error);
    return 2;
}

// Argument -> absolute, normalised directory ("a/b/", "a/b/." and "a/b" are alike).
std::filesystem::path toPath(const char* text) {
    auto path = std::filesystem::absolute(text).lexically_normal();
    if (!path.has_filename()) {
        path = path.parent_path();
    }
    return path;
}

// A dependency as read from a Lua table: pointers into Lua strings.
struct RawDependency {
    const char* name;
    const char* url;
    const char* ref;

    Project::Dependency get() const { return {name, url, ref}; }
};

// table[key] as a string. The value stays on the stack, which keeps the pointer
// valid, so call this AFTER reading every positional argument.
const char* stringField(lua_State* L, int table, const char* key, bool required) {
    lua_getfield(L, table, key);
    if (!required && lua_isnil(L, -1)) {
        return "";
    }

    const char* value = lua_tostring(L, -1);
    if (!value) {
        luaL_error(L, "dependency.%s must be a string", key);
    }
    return value;
}

RawDependency readDependency(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    const char* name = stringField(L, index, "name", true);
    const char* url = stringField(L, index, "url", true);
    const char* ref = stringField(L, index, "ref", false);
    return {name, url, ref};
}

// Opens the project around "start", gives a PackageManager to action() (which
// pushes its result), and returns that result plus the progress log.
template <typename Action>
int runManager(lua_State* L, const char* start, Action&& action) {
    return guarded(L, [&] {
        Project project = Project::find(toPath(start));
        PackageManager manager(project);

        std::vector<std::string> log;
        manager.setProgressCallback([&](const std::string& line) { log.push_back(line); });

        action(manager);
        push(L, log);
        return 2;
    });
}

// Bindings ////////////////////////////////////////////////////

// sonata.openProject(dir: string?): Project / sonata.findProject(start: string?): Project
int projectSnapshot(lua_State* L, bool search) {
    const char* start = luaL_optstring(L, 1, ".");

    return guarded(L, [&] {
        const auto path = toPath(start);
        push(L, search ? Project::find(path) : Project::open(path));
        return 1;
    });
}

int sonataOpenProject(lua_State* L) { return projectSnapshot(L, false); }
int sonataFindProject(lua_State* L) { return projectSnapshot(L, true); }

// sonata.validateProject(dir: string?): (boolean, string?)
int sonataValidateProject(lua_State* L) {
    const char* directory = luaL_optstring(L, 1, ".");

    return validation(L, [&]() -> std::optional<std::string> {
        Project::open(toPath(directory));
        return std::nullopt;
    });
}

// sonata.validateDependency(dep: Dependency): (boolean, string?)
int sonataValidateDependency(lua_State* L) {
    const RawDependency dependency = readDependency(L, 1);

    return validation(L, [&] { return dependency.get().validationError(); });
}

// install / installed / prune: (project: string?)
template <auto Method>
int managerCall(lua_State* L) {
    const char* project = luaL_optstring(L, 1, ".");

    return runManager(L, project, [&](PackageManager& manager) { push(L, (manager.*Method)()); });
}

// sonata.update(name: string?, project: string?)  (no name = update everything)
int sonataUpdate(lua_State* L) {
    const char* name = luaL_optstring(L, 1, "");
    const char* project = luaL_optstring(L, 2, ".");

    return runManager(L, project, [&](PackageManager& manager) {
        push(L, *name ? manager.update(name) : manager.update());
    });
}

// sonata.add(dep: Dependency, project: string?)
int sonataAdd(lua_State* L) {
    const char* project = luaL_optstring(L, 2, "."); // before readDependency, which pushes
    const RawDependency dependency = readDependency(L, 1);

    return runManager(L, project, [&](PackageManager& manager) {
        push(L, manager.add(dependency.get()));
    });
}

// sonata.remove(name: string, project: string?)
int sonataRemove(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* project = luaL_optstring(L, 2, ".");

    return runManager(L, project, [&](PackageManager& manager) { push(L, manager.remove(name)); });
}

void pushInfo(lua_State* L) {
    namespace build = sonata::info;

    lua_createtable(L, 0, 12);
    field(L, "name", build::name);
    field(L, "id", build::id);
    field(L, "binary", build::binary);
    field(L, "tagline", build::tagline);
    field(L, "version", build::version_string());
    field(L, "gitHash", build::git_hash);
    field(L, "luauVersion", build::luau_version);
    field(L, "buildType", build::build_type);
    field(L, "platform", build::platform_string());
    field(L, "compiler", build::compiler_string());
    flag(L, "debug", build::is_debug);

    lua_createtable(L, 0, 4);
    number(L, "major", build::version.major);
    number(L, "minor", build::version.minor);
    number(L, "patch", build::version.patch);
    field(L, "pre", build::version.pre);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "semver");

    lua_setreadonly(L, -1, 1); // the loader only locks the top-level table
}

constexpr NativeFunction kFunctions[] = {
    {"openProject", sonataOpenProject},
    {"findProject", sonataFindProject},
    {"validateProject", sonataValidateProject},
    {"validateDependency", sonataValidateDependency},
    {"install", managerCall<&PackageManager::install>},
    {"installed", managerCall<&PackageManager::installed>},
    {"prune", managerCall<&PackageManager::prune>},
    {"update", sonataUpdate},
    {"add", sonataAdd},
    {"remove", sonataRemove},
};

} // namespace

// require("@sonata/sonata")
void openSonata(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 1);
    setFunctions(L, kFunctions);

    pushInfo(L);
    lua_setfield(L, -2, "info");
}

} // namespace sonata::lib::libs