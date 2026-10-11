#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include <sonata/core/library.hpp>
#include <sonata/core/module.hpp>
#include <sonata/core/luau/datafile.hpp>
#include <sonata/core/project.hpp>
#include <sonata/core/scheduler.hpp>
#include "lua.h"
#include "lualib.h"

namespace fs = std::filesystem;

namespace sonata::luau {

namespace {

// Registry key of the table mapping module path -> module value
constexpr const char* kCacheKey = "sonata.modules";

constexpr const char* kSourceExtension = ".luau";
constexpr const char* kInitFile = "init.luau";
constexpr const char* kProjectFile = "project.luau";

// Relative to a package root. Same layout as Project / PackageManager.
constexpr const char* kManifestPath = ".sonata/project.luau";
constexpr const char* kDependenciesPath = ".sonata/deps";

// Built-in (C++) libraries are imported as require("@sonata/<name>"). The
// "sonata" alias is reserved for them: addAlias() refuses it, and a dependency
// with that name is never consulted (the built-in lookup comes first).
constexpr std::string_view kBuiltinAlias = "sonata";
constexpr std::string_view kBuiltinPrefix = "@sonata/";

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "/a/b/c.luau" -> "/a/b". "/c.luau" -> "" (callers append '/' themselves)
std::string_view directoryOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

// "/a/b" + "c" -> "/a/b/c", "/" + "c" -> "/c". "directory" is a normalised path.
std::string join(const std::string& directory, std::string_view name) {
    std::string result = directory;
    if (result.empty() || result.back() != '/') {
        result += '/';
    }
    result += name;
    return result;
}

// The folder (or file) containing "path", in normalised form:
//   "/a/b" -> "/a"    "/a" -> "/"    "C:/a" -> "C:/"
// std::nullopt if "path" is already a root ("/" or "C:/").
std::optional<std::string> parentDirectory(const std::string& path) {
    if (path.empty() || path.back() == '/') {
        return std::nullopt;
    }

    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }

    std::string parent = path.substr(0, slash);
    const bool driveLetter = parent.size() == 2 && parent[1] == ':' &&
                             std::isalpha(static_cast<unsigned char>(parent[0]));

    if (parent.empty() || driveLetter) {
        parent += '/';
    }

    return parent;
}

// Where the dependencies of the package at "packageRoot" are installed.
//
// Everything is installed flat in the root project's .sonata/deps, so a package
// that is itself sitting in a deps folder finds its dependencies right next to
// itself. Any other package is the root project, and has its own deps folder.
std::string dependenciesDirectoryOf(const std::string& packageRoot) {
    const std::string layout = std::string("/") + kDependenciesPath;

    if (const auto parent = parentDirectory(packageRoot); parent && endsWith(*parent, layout)) {
        return *parent;
    }

    return join(packageRoot, kDependenciesPath);
}

// Keeps "loading_" accurate even when we leave a function early.
class LoadGuard {
public:
    LoadGuard(std::vector<std::string>& stack, std::string path)
        : stack_(stack) {
        stack_.push_back(std::move(path));
    }

    ~LoadGuard() {
        stack_.pop_back();
    }

    LoadGuard(const LoadGuard&) = delete;
    LoadGuard& operator=(const LoadGuard&) = delete;

private:
    std::vector<std::string>& stack_;
};

// Empty string if "path" isn't currently loading.
std::string describeCycle(const std::vector<std::string>& loading, const std::string& path) {
    auto it = std::find(loading.begin(), loading.end(), path);
    if (it == loading.end()) {
        return {};
    }

    std::string chain;
    for (; it != loading.end(); ++it) {
        chain += *it;
        chain += " -> ";
    }
    chain += path;

    return "cyclic require: " + chain;
}

// Pushes the cached value for "path" and returns true, or pushes nothing.
bool pushCached(lua_State* L, const std::string& path) {
    lua_getfield(L, LUA_REGISTRYINDEX, kCacheKey);
    lua_getfield(L, -1, path.c_str());

    if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return false;
    }

    lua_remove(L, -2); // drop the cache table, keep the value
    return true;
}

// Stores the value on top of the stack in the cache (leaves it there).
void cacheTop(lua_State* L, const std::string& path) {
    lua_getfield(L, LUA_REGISTRYINDEX, kCacheKey);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, path.c_str());
    lua_pop(L, 1);
}

// The module path of the Lua code that called us, or "" if unknown.
//
// We walk past C frames so "pcall(require, "./x")" still resolves relative to
// the script that wrote the pcall. Only chunks we loaded ourselves count: their
// chunk name is "@<module path>".
std::string callerPath(lua_State* L) {
    lua_Debug ar;

    // Level 0 is `require` itself.
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

// Pushes "<caller file>:<line>: message" and returns -1 (see requireModule).
int failWithLocation(lua_State* L, const std::string& message) {
    luaL_where(L, 1);
    lua_pushlstring(L, message.data(), message.size());
    lua_concat(L, 2);
    return -1;
}

std::string errorMessage(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_tolstring(L, -1, &length);
    return std::string(text, length);
}

bool readModule(const ModuleSource& source, const std::string& path, Bytecode& out, std::string& error) {
    try {
        out = source.load(path);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    if (out.empty()) {
        error = "no bytecode was produced for module: " + path;
        return false;
    }

    return true;
}

// Loads "bytecode" into a fresh thread, ready to be resumed.
//
// The thread is left on top of L's stack. Returns false on a load failure, with
// the message on top of the thread's stack.
bool prepare(lua_State* L, const std::string& path, const Bytecode& bytecode, lua_State*& thread) {
    // Create the thread from the *main* thread so it doesn't inherit the
    // globals of whoever called require (a sandboxed module thread, say),
    // otherwise modules would see each other's global writes.
    lua_State* mainThread = lua_mainthread(L);
    thread = lua_newthread(mainThread);
    lua_xmove(mainThread, L, 1); // anchor it on L's stack so it can't be collected

    luaL_sandboxthread(thread);

    const std::string chunkName = "@" + path;
    return luau_load(thread, chunkName.c_str(), bytecode.luauData(), bytecode.size(), 0) == 0;
}

// Loads "bytecode" into a fresh thread and runs it to completion (or first yield).
//
// The thread is left on top of L's stack, and its results (or the error value)
// are on the thread's own stack. Returns the lua_resume status; a load failure
// is reported as LUA_ERRSYNTAX with the message on top of the thread's stack.
int launch(lua_State* L, const std::string& path, const Bytecode& bytecode, lua_State*& thread) {
    if (!prepare(L, path, bytecode, thread)) {
        return LUA_ERRSYNTAX;
    }
    return lua_resume(thread, L, 0);
}

// "@sonata" or "@sonata/<anything>"
bool isBuiltinRequest(std::string_view request) {
    return request == "@sonata" || startsWith(request, kBuiltinPrefix);
}

// "@sonata/path" -> "path". Empty for "@sonata" and "@sonata/".
std::string_view builtinName(std::string_view request) {
    return request.size() > kBuiltinPrefix.size() ? request.substr(kBuiltinPrefix.size()) : std::string_view{};
}

// Runs a library's opener. Called through lua_pcall so a library that raises
// an error fails the require() instead of tearing through the loader.
//
// Stack on entry: [library (lightuserdata)]. Returns the module value.
int openLibrary(lua_State* L) {
    const auto* library = static_cast<const lib::NativeLibrary*>(lua_tolightuserdata(L, 1));

    const int before = lua_gettop(L);
    library->open(L);

    if (lua_gettop(L) != before + 1) {
        luaL_error(L, "the opener of built-in library '%s' must push exactly one value", library->name.c_str());
    }

    // Scripts must not be able to overwrite path.join and friends.
    if (lua_istable(L, -1)) {
        lua_setreadonly(L, -1, true);
    }

    return 1;
}

// require("@sonata/<name>"). Same contract as requireModule(): pushes the
// value and returns 1, or pushes an error value and returns -1.
//
// "allowed", if set, is the sorted list of libraries scripts may use (see
// ModuleLoader::restrictLibraries()).
int requireBuiltin(
    lua_State* L,
    const lib::LibraryRegistry& libraries,
    const std::optional<std::vector<std::string>>& allowed,
    const std::string& hint,
    std::string_view request
) {
    const std::string_view name = builtinName(request);

    if (name.empty()) {
        return failWithLocation(L, "require '" + std::string(request) + "' is missing a library name (expected '@sonata/<name>')");
    }

    const auto isAllowed = [&](std::string_view n) {
        return !allowed || std::binary_search(allowed->begin(), allowed->end(), n);
    };

    const lib::NativeLibrary* library = libraries.find(name);

    // A library Sonata has but this binary was built without (a runtime made
    // for one program) reads the same as one the program didn't select.
    const std::vector<std::string>& known = lib::knownLibraryNames();
    const bool knownButMissing = library == nullptr && allowed &&
                                 std::binary_search(known.begin(), known.end(), name);

    if ((library != nullptr && !isAllowed(name)) || knownButMissing) {
        std::string error = "built-in library '" + std::string(request) + "' is not enabled for this program";
        if (!hint.empty()) {
            error += " (" + hint + ")";
        }
        return failWithLocation(L, error);
    }

    if (library == nullptr) {
        std::string error = "unknown built-in library '" + std::string(request) + "'";

        std::vector<std::string> available = libraries.names();
        available.erase(std::remove_if(available.begin(), available.end(),
                                       [&](const std::string& n) { return !isAllowed(n); }),
                        available.end());
        for (std::size_t i = 0; i < available.size(); ++i) {
            error += (i == 0) ? " (available: " : ", ";
            error += available[i];
        }
        if (!available.empty()) {
            error += ')';
        }

        return failWithLocation(L, error);
    }

    // Shares the module cache with file modules. Those are keyed by absolute
    // path, so "@sonata/..." can never collide with one.
    const std::string key = std::string(kBuiltinPrefix) + library->name;

    if (pushCached(L, key)) {
        return 1;
    }

    lua_pushcfunction(L, &openLibrary, "openLibrary");
    lua_pushlightuserdata(L, const_cast<lib::NativeLibrary*>(library));

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const std::string cause = errorMessage(L);
        lua_pop(L, 2); // the error value, and the string errorMessage() pushed

        return failWithLocation(L, "could not open built-in library '" + std::string(request) + "': " + cause);
    }

    cacheTop(L, key);
    return 1;
}

} // namespace


// Paths
std::optional<std::string> normalizePath(std::string_view input) {
    std::string path(input);
    std::replace(path.begin(), path.end(), '\\', '/');

    std::string root;
    std::size_t pos = 0;

    if (path.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':') {
        root = path.substr(0, 2) + "/";
        pos = 2;
    } else if (!path.empty() && path[0] == '/') {
        root = "/";
    } else {
        return std::nullopt; // not absolute
    }

    std::vector<std::string_view> parts;

    while (pos <= path.size()) {
        std::size_t next = path.find('/', pos);
        if (next == std::string::npos) {
            next = path.size();
        }

        const std::string_view part(path.data() + pos, next - pos);

        if (part.empty() || part == ".") {
            // nothing
        } else if (part == "..") {
            if (parts.empty()) {
                return std::nullopt; // would climb above the root
            }
            parts.pop_back();
        } else {
            parts.push_back(part);
        }

        pos = next + 1;
    }

    std::string result = root;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            result += '/';
        }
        result += parts[i];
    }

    return result;
}


// Sources
bool FileSystemSource::exists(std::string_view path) const {
    std::error_code ec;
    return fs::is_regular_file(fs::path(std::string(path)), ec);
}

Bytecode FileSystemSource::load(std::string_view path) const {
    // Note: on Windows, fs::path(std::string) assumes the ANSI code page. If you
    // need non-ASCII project paths there, convert from UTF-8 here.
    std::ifstream file(fs::path(std::string(path)), std::ios::binary);
    if (!file) {
        throw ModuleError("could not open module: " + std::string(path));
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    return compiler_.compile(contents.str());
}

std::optional<std::vector<std::string>> FileSystemSource::dependencyNames(std::string_view directory) const {
    const std::string root(directory);

    // Cheap "no" for the (common) case of a folder that isn't a package.
    if (!exists(join(root, kManifestPath))) {
        return std::nullopt;
    }

    // Project::open() is what installed packages were validated with, so the
    // names it hands back have already passed the dependency rules.
    try {
        const Project project = Project::open(fs::path(root));

        std::vector<std::string> names;
        for (const Project::Dependency& dependency : project.manifest().dependencies) {
            names.push_back(dependency.name);
        }

        return names;
    } catch (const std::exception& e) {
        throw ModuleError("could not read the manifest of " + root + ": " + e.what());
    }
}

std::optional<std::string> FileSystemSource::packageEntrypoint(std::string_view directory) const {
    const std::string manifest = join(std::string(directory), kManifestPath);

    if (!exists(manifest)) {
        return std::nullopt;
    }

    // Only the one field is needed, so this deliberately doesn't go through
    // Project::open() (which would also insist the entrypoint exists).
    DataValue data;
    try {
        data = DataFile::parseFile(fs::path(manifest));
    } catch (const std::exception& e) {
        throw ModuleError("could not read the manifest of " + std::string(directory) + ": " + e.what());
    }

    const DataValue* entrypoint = data.find("entrypoint");
    if (entrypoint == nullptr || !entrypoint->isString()) {
        return std::nullopt;
    }

    return entrypoint->asString();
}

void MemorySource::add(std::string_view path, Bytecode bytecode) {
    std::string rooted(path);
    if (rooted.empty() || (rooted[0] != '/' && rooted[0] != '\\')) {
        rooted.insert(0, "/");
    }

    auto normalized = normalizePath(rooted);
    if (!normalized) {
        throw std::invalid_argument("module path escapes the root: " + std::string(path));
    }

    files_[std::move(*normalized)] = std::move(bytecode);
}

void MemorySource::addPackage(std::string_view directory, std::vector<std::string> dependencies, std::string entrypoint) {
    std::string rooted(directory);
    if (rooted.empty() || (rooted[0] != '/' && rooted[0] != '\\')) {
        rooted.insert(0, "/");
    }

    auto normalized = normalizePath(rooted);
    if (!normalized) {
        throw std::invalid_argument("package directory escapes the root: " + std::string(directory));
    }

    packages_[std::move(*normalized)] = Package{std::move(dependencies), std::move(entrypoint)};
}

std::optional<std::vector<std::string>> MemorySource::dependencyNames(std::string_view directory) const {
    auto it = packages_.find(std::string(directory));
    if (it == packages_.end()) {
        return std::nullopt;
    }

    return it->second.dependencies;
}

std::optional<std::string> MemorySource::packageEntrypoint(std::string_view directory) const {
    auto it = packages_.find(std::string(directory));
    if (it == packages_.end() || it->second.entrypoint.empty()) {
        return std::nullopt;
    }

    return it->second.entrypoint;
}

bool MemorySource::exists(std::string_view path) const {
    return files_.find(std::string(path)) != files_.end();
}

Bytecode MemorySource::load(std::string_view path) const {
    auto it = files_.find(std::string(path));
    if (it == files_.end()) {
        throw ModuleError("module not found: " + std::string(path));
    }

    return it->second;
}


// ModuleLoader
ModuleLoader::ModuleLoader(std::unique_ptr<ModuleSource> source)
    : source_(std::move(source)) {
    if (!source_) {
        throw std::invalid_argument("ModuleLoader needs a ModuleSource");
    }

    registerBuiltinLibraries(libraries_);
}

void ModuleLoader::addLibrary(lib::NativeLibrary library) {
    libraries_.add(std::move(library));
}

void ModuleLoader::restrictLibraries(const std::vector<std::string>& allowed, std::string hint) {
    std::vector<std::string> sorted = allowed;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    allowedLibraries_ = std::move(sorted);
    restrictionHint_ = std::move(hint);
}

void ModuleLoader::addAlias(std::string name, std::string_view directory) {
    if (name.empty() || name.find('/') != std::string::npos) {
        throw std::invalid_argument("invalid alias name: '" + name + "'");
    }

    if (name == kBuiltinAlias) {
        throw std::invalid_argument("the alias '@sonata' is reserved for built-in libraries");
    }

    auto normalized = normalizePath(directory);
    if (!normalized) {
        throw std::invalid_argument(
            "alias '" + name + "' needs an absolute path, got: " + std::string(directory)
        );
    }

    aliases_[std::move(name)] = std::move(*normalized);
}

void ModuleLoader::install(VM& vm) {
    lua_State* L = vm.state();

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kCacheKey);

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, &ModuleLoader::requireCallback, "require", 1);
    lua_setglobal(L, "require");
}

void ModuleLoader::run(VM& vm, std::string_view entry) {
    const std::optional<std::string> path = normalizePath(entry);
    if (!path) {
        throw ModuleError("entry must be an absolute path: " + std::string(entry));
    }

    if (!source_->exists(*path)) {
        throw ModuleError("entry script not found: " + *path);
    }

    Bytecode bytecode;
    std::string error;
    if (!readModule(*source_, *path, bytecode, error)) {
        throw ModuleError(error);
    }

    lua_State* L = vm.state();

    // Registering the entry as "loading" means a dependency that requires it
    // back gets a proper cycle error instead of running it a second time.
    LoadGuard guard(loading_, *path);

    lua_State* thread = nullptr;
    if (!prepare(L, *path, bytecode, thread)) {
        std::string message = errorMessage(thread);
        lua_pop(L, 1); // the thread
        throw ModuleError(std::move(message));
    }

    // The entry runs as the scheduler's first task, so it can wait (task.wait,
    // sockets, child processes, ...) while other tasks run. The program lasts
    // until nothing is left to do, or until the entry fails.
    sched::Scheduler& scheduler = sched::Scheduler::of(L);
    scheduler.setCritical(thread);
    scheduler.resume(thread, L, 0);
    scheduler.run();
    scheduler.setCritical(nullptr);

    std::optional<std::string> failure = scheduler.fatalError();
    if (!failure && lua_costatus(L, thread) == LUA_COSUS && !scheduler.isWaiting(thread)) {
        // It used coroutine.yield() at the top level: nothing will ever resume it.
        failure = std::string("thread yielded unexpectedly\nstacktrace:\n") + lua_debugtrace(thread);
    }

    lua_pop(L, 1); // the thread
    if (failure) {
        throw ModuleError(std::move(*failure));
    }
}

// The function Lua actually calls. It deliberately holds no C++ objects with
// destructors: lua_error() may longjmp depending on how Luau was built, so all
// the real work happens in requireModule() and this just raises its result.
int ModuleLoader::requireCallback(lua_State* L) {
    auto* self = static_cast<ModuleLoader*>(lua_tolightuserdata(L, lua_upvalueindex(1)));

    std::size_t length = 0;
    const char* request = luaL_checklstring(L, 1, &length);

    const int results = self->requireModule(L, std::string_view(request, length));
    if (results < 0) {
        lua_error(L); // error value is on top of the stack
    }

    return results;
}

int ModuleLoader::requireModule(lua_State* L, std::string_view request) {
    // Built-in libraries are checked before anything else, so "@sonata" can't be shadowed.
    if (isBuiltinRequest(request)) {
        return requireBuiltin(L, libraries_, allowedLibraries_, restrictionHint_, request);
    }

    std::string path;
    std::string error;

    if (!resolve(request, callerPath(L), path, error)) {
        return failWithLocation(L, error);
    }

    if (pushCached(L, path)) {
        return 1;
    }

    if (const std::string cycle = describeCycle(loading_, path); !cycle.empty()) {
        return failWithLocation(L, cycle);
    }

    Bytecode bytecode;
    if (!readModule(*source_, path, bytecode, error)) {
        return failWithLocation(L, error);
    }

    LoadGuard guard(loading_, path);

    lua_State* thread = nullptr;
    const int status = launch(L, path, bytecode, thread);

    if (status == LUA_YIELD) {
        return failWithLocation(L, "module yielded while loading (modules can't yield): " + path);
    }

    if (status != LUA_OK) {
        // Pass the module's own error along untouched. It may not be a string.
        lua_xmove(thread, L, 1);
        return -1;
    }

    if (lua_gettop(thread) != 1 || lua_isnil(thread, 1)) {
        return failWithLocation(L, "module must return exactly one non-nil value: " + path);
    }

    lua_xmove(thread, L, 1); // [thread, value]
    lua_remove(L, -2);       // [value]
    cacheTop(L, path);

    return 1;
}

auto ModuleLoader::packageOf(const std::string& file) const -> std::optional<Package> {
    // Walk up from the file's folder to the first one holding a manifest.
    for (std::optional<std::string> directory = parentDirectory(file);
         directory;
         directory = parentDirectory(*directory)) {
        auto it = manifests_.find(*directory);

        if (it == manifests_.end()) {
            // If this throws nothing gets cached, so the next require tries again.
            it = manifests_.emplace(*directory, source_->dependencyNames(*directory)).first;
        }

        if (it->second) {
            return Package{*directory, *it->second};
        }
    }

    return std::nullopt;
}

bool ModuleLoader::resolveAlias(
    const std::string& name,
    std::string_view request,
    const std::string& caller,
    std::string& directory,
    std::string& error
) const {
    // 1. Explicit registrations always win.
    if (const auto alias = aliases_.find(name); alias != aliases_.end()) {
        directory = alias->second;
        return true;
    }

    // 2. The manifest of the package the requiring file belongs to.
    std::optional<Package> owner;

    if (!caller.empty()) {
        try {
            owner = packageOf(caller);
        } catch (const std::exception& e) {
            error = "can't resolve require '" + std::string(request) + "': " + e.what();
            return false;
        }
    }

    if (owner) {
        const std::vector<std::string>& declared = owner->dependencies;

        if (std::find(declared.begin(), declared.end(), name) != declared.end()) {
            directory = join(dependenciesDirectoryOf(owner->root), name);
            return true;
        }
    }

    error = "unknown alias '@" + name + "' in require '" + std::string(request) + "'";
    if (owner) {
        error += " (it isn't registered, and " + join(owner->root, kManifestPath) +
                 " doesn't declare a dependency called '" + name + "')";
    }

    return false;
}

bool ModuleLoader::resolve(
    std::string_view request,
    const std::string& caller,
    std::string& path,
    std::string& error
) const {
    const std::string shown(request);

    if (request.empty()) {
        error = "require path is empty";
        return false;
    }

    std::string target;

    if (request.front() == '@') {
        const std::size_t slash = request.find('/');
        const std::string name(request.substr(1, slash == std::string_view::npos ? slash : slash - 1));

        std::string directory;
        if (!resolveAlias(name, shown, caller, directory, error)) {
            return false;
        }

        target = directory;
        if (slash != std::string_view::npos) {
            target += '/';
            target += request.substr(slash + 1);
        }
    } else if (request == "." || request == ".." || startsWith(request, "./") || startsWith(request, "../")) {
        if (caller.empty()) {
            error = "can't resolve relative require '" + shown + "': the calling script isn't a known module";
            return false;
        }

        // Relative paths start at the directory of the requiring file. That
        // includes init.luau: "./helper" inside lib/init.luau means lib/helper.
        target = std::string(directoryOf(caller));
        target += '/';
        target += request;
    } else {
        error = "require path must start with './', '../' or '@' (got '" + shown + "')";
        return false;
    }

    const std::optional<std::string> normalized = normalizePath(target);
    if (!normalized) {
        error = "require '" + shown + "' climbs above the project root";
        return false;
    }

    // Candidates: <base>.luau and <base>/init.luau. If <base> is a package
    // whose manifest names an entrypoint (a .luau file that exists), that
    // file takes init.luau's place.
    const std::string& base = *normalized;
    const bool atRoot = base.back() == '/'; // "/" or "C:/" can't have a ".luau" sibling

    const std::string asFile = base + kSourceExtension;
    std::string asInit = atRoot ? base + kInitFile : base + "/" + kInitFile;

    std::optional<std::string> entrypoint;
    try {
        auto it = entrypoints_.find(base);
        if (it == entrypoints_.end()) {
            it = entrypoints_.emplace(base, source_->packageEntrypoint(base)).first;
        }
        entrypoint = it->second;
    } catch (const std::exception& e) {
        error = "can't resolve require '" + shown + "': " + e.what();
        return false;
    }

    if (entrypoint && endsWith(*entrypoint, kSourceExtension)) {
        const std::optional<std::string> candidate =
            normalizePath(atRoot ? base + *entrypoint : base + "/" + *entrypoint);

        // Make sure the file actually exists (and stays inside the package)
        // before committing to it.
        const std::string inside = atRoot ? base : base + "/";
        if (candidate && startsWith(*candidate, inside) && source_->exists(*candidate)) {
            asInit = *candidate;
        }
    }

    const bool fileExists = !atRoot && source_->exists(asFile);
    const bool initExists = source_->exists(asInit);

    if (fileExists && initExists) {
        error = "ambiguous require '" + shown + "': both " + asFile + " and " + asInit + " exist";
        return false;
    }

    if (!fileExists && !initExists) {
        error = "module not found: '" + shown + "' (looked for " +
                (atRoot ? asInit : asFile + " and " + asInit) + ")";
        return false;
    }

    path = fileExists ? asFile : asInit;
    return true;
}

} // namespace sonata::luau