#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace sonata {

/*
 * POCKET
 *
 * Read-only files that travel with a program: whatever a package lists in
 * build.assets (files or whole folders, relative to the package root).
 * Scripts read them through require("@sonata/pocket"):
 *
 *     local pocket = require("@sonata/pocket")
 *     local config = pocket.read("assets/config.json")
 *
 * "sn run" reads them from disk, "sn build" packs them into the executable.
 * Either way only the declared files are visible, so a program can't come to
 * depend on a file that won't be packed.
 *
 * Every package (the project and each dependency) has its own assets, and a
 * script sees the ones of the package it belongs to, so a dependency's
 * "assets/x" never collides with the project's.
 *
 * Packages are identified by the module path of their root folder (see
 * module.hpp): "/" and "/.sonata/deps/foo" in a built program, real absolute
 * paths under "sn run". Asset paths are relative to that root, use '/', and
 * never contain "." or ".." segments.
 */
class Pocket {
public:
    virtual ~Pocket() = default;

    /* The contents of an asset, or nullopt if the package has no such asset. */
    virtual std::optional<std::string> read(const std::string& package, const std::string& path) const = 0;

    /* Every asset of the package, sorted. */
    virtual std::vector<std::string> list(const std::string& package) const = 0;

    /* True when the assets come from a built executable. */
    virtual bool bundled() const = 0;

    /*
     * The package that contains the module "modulePath" (the innermost one),
     * or the first package added if none does.
     */
    std::optional<std::string> packageOf(std::string_view modulePath) const;

    /*
     * Turns a path written by a script into the canonical asset form:
     * "./assets\\a.txt" -> "assets/a.txt". nullopt if it's empty, absolute, or
     * climbs out of the package.
     */
    static std::optional<std::string> normalize(std::string_view path);

    /*
     * Makes "pocket" the one @sonata/pocket uses in this VM. The pocket must
     * outlive the VM. Without one, every pocket function raises an error.
     */
    static void install(lua_State* L, const Pocket* pocket);
    static const Pocket* of(lua_State* L);

protected:
    void addRoot(std::string package);

private:
    std::vector<std::string> roots_; // in the order they were added
};

/* "sn run": reads the declared files from each package's folder on demand. */
class DiskPocket final : public Pocket {
public:
    /* "assets" is the package's build.assets. */
    void addPackage(const std::string& package, std::filesystem::path directory,
                    const std::vector<std::string>& assets);

    std::optional<std::string> read(const std::string& package, const std::string& path) const override;
    std::vector<std::string> list(const std::string& package) const override;
    bool bundled() const override { return false; }

private:
    struct Package {
        std::filesystem::path directory;
        std::vector<std::string> assets; // normalised
    };

    std::map<std::string, Package> packages_;
};

/* A built program: everything was read into memory from the executable. */
class MemoryPocket final : public Pocket {
public:
    void addPackage(const std::string& package);
    void add(const std::string& package, const std::string& path, std::string data);

    std::optional<std::string> read(const std::string& package, const std::string& path) const override;
    std::vector<std::string> list(const std::string& package) const override;
    bool bundled() const override { return true; }

private:
    std::map<std::string, std::map<std::string, std::string>> packages_;
};

/*
 * Expands build.assets for the package in "directory": every regular file the
 * entries name (folders recursively, skipping any .sonata folder), as asset
 * paths, sorted and without duplicates. Throws std::runtime_error if an entry
 * doesn't exist.
 */
std::vector<std::string> collectAssets(const std::filesystem::path& directory,
                                       const std::vector<std::string>& assets);

} // namespace sonata
