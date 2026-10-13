#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <sonata/core/module.hpp>
#include <sonata/core/pocket.hpp>
#include "lua.h"

namespace fs = std::filesystem;

namespace sonata {

namespace {

// Registry key holding the VM's Pocket (lightuserdata).
constexpr const char* kPocketKey = "sonata.pocket";

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// "assets" covers "assets" itself and everything under "assets/".
bool covers(const std::string& asset, const std::string& path) {
    return path == asset || (startsWith(path, asset) && path.size() > asset.size() && path[asset.size()] == '/');
}

std::string toAssetPath(const fs::path& relative) {
    return relative.generic_string();
}

} // namespace

// Pocket
std::optional<std::string> Pocket::normalize(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\') {
        return std::nullopt;
    }

    // "C:..." is a drive, not a relative path.
    if (path.size() >= 2 && path[1] == ':') {
        return std::nullopt;
    }

    std::optional<std::string> normal = luau::normalizePath("/" + std::string(path));
    if (!normal || *normal == "/") {
        return std::nullopt;
    }

    return normal->substr(1);
}

std::optional<std::string> Pocket::packageOf(std::string_view modulePath) const {
    const std::string* best = nullptr;

    for (const std::string& root : roots_) {
        const bool inside = root.back() == '/'
            ? startsWith(modulePath, root)
            : startsWith(modulePath, root) && modulePath.size() > root.size() && modulePath[root.size()] == '/';

        if (inside && (best == nullptr || root.size() > best->size())) {
            best = &root;
        }
    }

    if (best != nullptr) {
        return *best;
    }

    if (!roots_.empty()) {
        return roots_.front();
    }

    return std::nullopt;
}

void Pocket::addRoot(std::string package) {
    if (std::find(roots_.begin(), roots_.end(), package) == roots_.end()) {
        roots_.push_back(std::move(package));
    }
}

void Pocket::install(lua_State* L, const Pocket* pocket) {
    lua_pushlightuserdata(L, const_cast<Pocket*>(pocket));
    lua_setfield(L, LUA_REGISTRYINDEX, kPocketKey);
}

const Pocket* Pocket::of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kPocketKey);
    const auto* pocket = static_cast<const Pocket*>(lua_tolightuserdata(L, -1));
    lua_pop(L, 1);
    return pocket;
}

// DiskPocket
void DiskPocket::addPackage(const std::string& package, fs::path directory, const std::vector<std::string>& assets) {
    Package entry;
    entry.directory = std::move(directory);

    for (const std::string& asset : assets) {
        if (std::optional<std::string> normal = normalize(asset)) {
            entry.assets.push_back(std::move(*normal));
        }
    }

    packages_[package] = std::move(entry);
    addRoot(package);
}

std::optional<std::string> DiskPocket::read(const std::string& package, const std::string& path) const {
    const auto it = packages_.find(package);
    if (it == packages_.end()) {
        return std::nullopt;
    }

    const bool declared = std::any_of(it->second.assets.begin(), it->second.assets.end(),
                                      [&](const std::string& asset) { return covers(asset, path); });

    // .sonata is never part of a package's assets (see collectAssets()).
    if (!declared || path == ".sonata" || startsWith(path, ".sonata/") || path.find("/.sonata/") != std::string::npos) {
        return std::nullopt;
    }

    const fs::path file = it->second.directory / fs::path(path);

    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::nullopt;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not read " + file.generic_string());
    }

    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

std::vector<std::string> DiskPocket::list(const std::string& package) const {
    const auto it = packages_.find(package);
    if (it == packages_.end()) {
        return {};
    }

    return collectAssets(it->second.directory, it->second.assets);
}

// MemoryPocket
void MemoryPocket::addPackage(const std::string& package) {
    packages_[package];
    addRoot(package);
}

void MemoryPocket::add(const std::string& package, const std::string& path, std::string data) {
    packages_[package][path] = std::move(data);
    addRoot(package);
}

std::optional<std::string> MemoryPocket::read(const std::string& package, const std::string& path) const {
    const auto it = packages_.find(package);
    if (it == packages_.end()) {
        return std::nullopt;
    }

    const auto file = it->second.find(path);
    if (file == it->second.end()) {
        return std::nullopt;
    }

    return file->second;
}

std::vector<std::string> MemoryPocket::list(const std::string& package) const {
    std::vector<std::string> result;

    const auto it = packages_.find(package);
    if (it != packages_.end()) {
        for (const auto& entry : it->second) {
            result.push_back(entry.first);
        }
    }

    return result; // std::map is already sorted
}

// collectAssets
std::vector<std::string> collectAssets(const fs::path& directory, const std::vector<std::string>& assets) {
    std::vector<std::string> result;

    for (const std::string& asset : assets) {
        const std::optional<std::string> normal = Pocket::normalize(asset);
        if (!normal) {
            throw std::runtime_error("build.assets entry '" + asset + "' is not a path inside the package");
        }

        const fs::path target = directory / fs::path(*normal);

        std::error_code ec;
        if (fs::is_regular_file(target, ec)) {
            result.push_back(*normal);
            continue;
        }

        if (!fs::is_directory(target, ec)) {
            throw std::runtime_error("build.assets entry '" + asset + "' does not exist (" +
                                     target.generic_string() + ")");
        }

        for (auto it = fs::recursive_directory_iterator(target, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_directory(ec) && it->path().filename() == ".sonata") {
                it.disable_recursion_pending();
                continue;
            }

            if (it->is_regular_file(ec)) {
                result.push_back(toAssetPath(it->path().lexically_relative(directory)));
            }
        }

        if (ec) {
            throw std::runtime_error("could not list " + target.generic_string() + ": " + ec.message());
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    // An entry naming ".sonata" itself (or a file in it) is never an asset.
    result.erase(std::remove_if(result.begin(), result.end(),
                                [](const std::string& path) {
                                    return path == ".sonata" || startsWith(path, ".sonata/") ||
                                           path.find("/.sonata/") != std::string::npos;
                                }),
                 result.end());

    return result;
}

} // namespace sonata
