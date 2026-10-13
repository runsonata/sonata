#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sonata/core/luau/compiler.hpp>

/*
 * BUNDLES
 *
 * A program made by "sn build" is the Sonata runtime (snrt) with a bundle
 * appended to it:
 *
 *     [ runtime executable ][ payload ][ trailer (32 bytes) ]
 *
 * At startup the runtime reads its own file, finds the trailer at the very
 * end, and runs the program in the payload. Loaders ignore data after the end
 * of an executable, so the runtime itself is untouched.
 *
 * Trailer, little-endian:
 *
 *     u64  stored payload size (bytes right before the trailer)
 *     u64  payload size once decompressed
 *     u32  CRC-32 of the stored payload
 *     u32  flags (bit 0: payload is zlib-compressed)
 *     8    magic "SNBUNDLE"
 *
 * Payload (after decompression): "SNPK", u32 format version, then the fields
 * of Bundle in declaration order. Strings and byte blobs are a u32 length
 * followed by the bytes, lists a u32 count followed by the items.
 *
 * Paths in a bundle are module paths under a virtual root: the project is
 * "/", its files "/main.luau", "/src/util.luau", and dependency "foo" is
 * "/.sonata/deps/foo" (the same layout as on disk).
 */
namespace sonata::bundle {

class BundleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Module {
    std::string path; // e.g. "/src/util.luau"
    luau::Bytecode bytecode;
};

struct Package {
    std::string root;                      // "/" or "/.sonata/deps/<name>"
    std::vector<std::string> dependencies; // names its manifest declares
    std::string entrypoint;                // as written in its manifest
};

struct Asset {
    std::string package; // root of the package it belongs to
    std::string path;    // relative to that root, e.g. "assets/logo.png"
    std::string data;
};

struct Bundle {
    std::string name;    // manifest name
    std::string version; // manifest version
    std::string entry;   // module path of the entry script

    // Built-in libraries the program may require, sorted.
    std::vector<std::string> libraries;
    // What to tell a script that requires any other one.
    std::string libraryHint;

    // "@name" -> directory, registered like PackageManager::registerAliases().
    std::vector<std::pair<std::string, std::string>> aliases;

    std::vector<Package> packages;
    std::vector<Module> modules;
    std::vector<Asset> assets;
};

/* The bytes to append to a runtime: payload plus trailer. */
std::string encode(const Bundle& bundle, bool compress);

/*
 * Reads the bundle at the end of "executable". std::nullopt if there is none
 * (a plain runtime); throws BundleError if there is one but it's damaged.
 */
std::optional<Bundle> read(const std::filesystem::path& executable);

/*
 * Writes "runtime" followed by the bundle to "output" (replacing it), and
 * marks it executable. Throws BundleError, including when "runtime" already
 * carries a bundle.
 */
void write(const std::filesystem::path& runtime, const std::filesystem::path& output,
           const Bundle& bundle, bool compress);

} // namespace sonata::bundle
