#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/luau/vm.hpp>
#include <sonata/core/library.hpp>

namespace sonata::luau {

// Raised for failures the module system detects itself: a module that can't be
// read or compiled, or an entry script that errors inside ModuleLoader::run().
class ModuleError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/*
 * MODULE PATHS
 *
 * Every module is identified by a module path: an absolute path written with
 * forward slashes, e.g.
 *
 *     /home/me/project/src/util.luau
 *     C:/project/src/util.luau
 *
 * "sn run"   uses the real absolute paths on disk.
 * "sn build" uses the same tree under a virtual root: /main.luau,
 *            /src/util.luau, /.sonata/deps/foo/init.luau, ...
 */

/*
 * Lexically normalises an absolute path: converts '\' to '/', drops "." and
 * empty segments, resolves "..".
 *
 * Returns std::nullopt if the path isn't absolute, or if ".." would climb above
 * its root.
 */
[[nodiscard]]
std::optional<std::string> normalizePath(std::string_view path);

/*
 * Where module bytecode comes from. The loader never touches the disk (or the
 * packed archive) directly, it only talks to one of these.
 *
 * Paths passed in are always normalised module paths (see above).
 */
class ModuleSource {
  public:
    virtual ~ModuleSource() = default;

    /* True if a module file exists at exactly this path. */
    virtual bool exists(std::string_view path) const = 0;

    /* Returns the module's bytecode. Throws (ModuleError ideally) on failure.
     */
    virtual Bytecode load(std::string_view path) const = 0;

    /*
     * The names of the dependencies declared in the manifest of the package
     * rooted at "directory", i.e. in <directory>/.sonata/project.luau. This is
     * what lets "@name" work inside a package for the dependencies that
     * package declares itself (see ModuleLoader).
     *
     * Returns std::nullopt if "directory" isn't a package root. Throws
     * (ModuleError ideally) if there is a manifest but it can't be read.
     *
     * The default says "not a package" for every directory, so a source that
     * doesn't override this keeps working; it just only gets the aliases that
     * were registered with ModuleLoader::addAlias().
     */
    virtual std::optional<std::vector<std::string>>
    dependencyNames(std::string_view directory) const {
        (void)directory;
        return std::nullopt;
    }

    /*
     * The "entrypoint" the manifest of the package rooted at "directory"
     * names, exactly as written (e.g. "src/init.luau"). Requiring the folder
     * itself loads that file instead of init.luau.
     *
     * std::nullopt if "directory" isn't a package root or its manifest doesn't
     * name one. Like dependencyNames(), the default says "not a package".
     */
    virtual std::optional<std::string>
    packageEntrypoint(std::string_view directory) const {
        (void)directory;
        return std::nullopt;
    }
};

/*
 * sn run: reads .luau files from disk and compiles them on demand.
 */
class FileSystemSource final : public ModuleSource {
  public:
    bool exists(std::string_view path) const override;
    Bytecode load(std::string_view path) const override;

    /* Reads <directory>/.sonata/project.luau through Project::open(). */
    std::optional<std::vector<std::string>>
    dependencyNames(std::string_view directory) const override;

    /* Reads "entrypoint" from <directory>/.sonata/project.luau. */
    std::optional<std::string>
    packageEntrypoint(std::string_view directory) const override;

  private:
    Compiler compiler_;
};

/*
 * Precompiled modules held in memory, keyed by virtual path.
 *
 * "sn build" can populate one of these from the packed archive at startup, or
 * you can implement ModuleSource directly on top of your archive reader
 * instead. Handy for tests either way.
 */
class MemorySource final : public ModuleSource {
  public:
    /* "path" is rooted at "/", so add("src/util.luau", ...) and
       add("/src/util.luau", ...) are equivalent. Throws std::invalid_argument
       if the path climbs above the root. */
    void add(std::string_view path, Bytecode bytecode);

    /* Records that the folder "directory" (rooted at "/", like add()) is a
       package whose manifest declares these dependencies, and (if not empty)
       this entrypoint. Use "/" for the project itself and e.g.
       "/.sonata/deps/foo" for a dependency. Throws std::invalid_argument if
       the path climbs above the root. */
    void addPackage(std::string_view directory,
                    std::vector<std::string> dependencies,
                    std::string entrypoint = {});

    bool exists(std::string_view path) const override;
    Bytecode load(std::string_view path) const override;
    std::optional<std::vector<std::string>>
    dependencyNames(std::string_view directory) const override;
    std::optional<std::string>
    packageEntrypoint(std::string_view directory) const override;

  private:
    struct Package {
        std::vector<std::string> dependencies;
        std::string entrypoint;
    };

    std::unordered_map<std::string, Bytecode> files_;
    std::unordered_map<std::string, Package> packages_;
};

/*
 * Implements "require" for one VM.
 *
 * Accepted require strings:
 *
 *     require("./helper")         relative to the requiring file's directory
 *     require("../shared/util")
 *     require("@name/path")       through an alias registered with addAlias()
 *     require("@name")
 *
 * A path P resolves to P.luau, or P/init.luau if that doesn't exist. If both
 * exist the require is an error. Anything not starting with "./", "../" or "@"
 * is rejected (this keeps bare names free for a future package mechanism).
 *
 * WHERE "@name" POINTS
 *
 * The alias is looked up in this order:
 *
 *   1. Aliases registered with addAlias(). These always win, so anything that
 *      resolved before still resolves to the same place.
 *   2. The dependencies declared in the manifest (.sonata/project.luau) of the
 *      package that contains the requiring file, i.e. the nearest folder above
 *      it that has a .sonata/project.luau. The dependency is looked up in the
 *      flat deps folder (see package.hpp): next to the package if the package
 *      itself sits in a .sonata/deps folder, otherwise in its own .sonata/deps.
 *
 * (2) means a dependency can require the packages its own manifest lists even
 * if the root project's manifest doesn't mention them, and without anyone
 * having to register an alias for them. Which manifest applies depends on the
 * file doing the require, not on the file being required. Packages are not
 * isolated from each other: a name that is registered (1) is visible
 * everywhere, and since everything is installed flat, a name always means the
 * same folder no matter who asks. How manifests are read is up to the
 * ModuleSource (see dependencyNames()).
 *
 * A module runs once per VM, on its own sandboxed thread, and must return
 * exactly one non-nil value. That value is cached and returned by every later
 * require of the same file. Cyclic requires are reported as errors.
 *
 * LIFETIME: install() gives the VM a raw pointer to this object, so the loader
 * must outlive every piece of Lua code that can run, and must not be moved.
 * Use one loader per VM, and declare it before the VM.
 */
class ModuleLoader {
  public:
    explicit ModuleLoader(std::unique_ptr<ModuleSource> source);

    ModuleLoader(const ModuleLoader &) = delete;
    ModuleLoader &operator=(const ModuleLoader &) = delete;

    /*
     * Registers "@name" as an alias for a directory (a module path).
     * Throws std::invalid_argument if the name or path is invalid.
     *
     * For dependencies: addAlias("foo", ".../.sonata/deps/foo"). An alias
     * registered here takes priority over the same name in a manifest.
     */
    void addAlias(std::string name, std::string_view directory);

    /*
     * Defines the global "require" on the VM. Call it before luaL_sandbox()
     * (or whatever makes your globals read-only).
     */
    void install(VM &vm);

    /*
     * Runs "entry" (a module path) as the program's entry point.
     *
     * The entry goes through the same machinery as required modules, so its
     * chunk name is its path and relative requires work from it. It may return
     * anything. Throws ModuleError, including a stack trace, if it fails.
     *
     * The entry runs as the first task of the VM's scheduler (see
     * sonata/core/scheduler.hpp), and run() keeps the scheduler going until
     * nothing is left to do: tasks the entry started outlive it. Errors in
     * those tasks are reported, not thrown; an error in the entry itself stops
     * everything.
     */
    void run(VM &vm, std::string_view entry);

    // Registers an extra built-in: require("@sonata/<name>")
    void addLibrary(lib::NativeLibrary library);

    /*
     * Only lets scripts require the built-in libraries named in "allowed";
     * requiring any other one fails with an error that ends in "hint" (e.g.
     * where to add the library). Used to enforce build.libraries, so that
     * "sn run" refuses exactly what a built program wouldn't have.
     *
     * Names that aren't registered are ignored. Without a call to this, every
     * registered library is available.
     */
    void restrictLibraries(const std::vector<std::string>& allowed,
                           std::string hint = {});

  private:
    static int requireCallback(lua_State *L);

    // Returns the number of results, or -1 with an error value on top of the
    // stack.
    int requireModule(lua_State *L, std::string_view request);

    bool resolve(std::string_view request, const std::string &caller,
                 std::string &path, std::string &error) const;

    // A folder with a .sonata/project.luau, and the dependencies it declares.
    struct Package {
        std::string root; // normalised module path of the folder
        std::vector<std::string> dependencies;
    };

    // Finds the directory "@name" stands for when required from "caller".
    bool resolveAlias(const std::string &name, std::string_view request,
                      const std::string &caller, std::string &directory,
                      std::string &error) const;

    // The package that contains the module "file", if any. May throw if its
    // manifest can't be read.
    std::optional<Package> packageOf(const std::string &file) const;

    lib::LibraryRegistry libraries_;
    std::optional<std::vector<std::string>> allowedLibraries_; // sorted
    std::string restrictionHint_;

    std::unique_ptr<ModuleSource> source_;
    std::unordered_map<std::string, std::string> aliases_;
    std::vector<std::string>
        loading_; // modules currently executing, outermost first

    // Manifests already looked at, by folder. nullopt = not a package root.
    // Filled on demand, so resolve() can stay const.
    mutable std::unordered_map<std::string,
                               std::optional<std::vector<std::string>>>
        manifests_;

    // Package entrypoints already looked up, by folder (see packageEntrypoint()).
    mutable std::unordered_map<std::string, std::optional<std::string>>
        entrypoints_;
};

} // namespace sonata::luau