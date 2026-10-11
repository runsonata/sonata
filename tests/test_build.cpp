// tests/test_build.cpp
#include "helper.hpp"
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sonata/core/bundle.hpp>
#include <sonata/core/library.hpp>
#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/luau/datafile.hpp>
#include <sonata/core/module.hpp>
#include <sonata/core/pocket.hpp>
#include <sonata/core/project.hpp>
#include <sonata/core/runtime.hpp>
#include <sonata/core/selection.hpp>

namespace fs = std::filesystem;
namespace luau = sonata::luau;

namespace {

// A minimal project: <dir>/.sonata/project.luau with "manifest" and a main.luau.
void writeProject(const TempDir &td, const fs::path &dir, std::string_view manifest) {
    td.write(dir / ".sonata" / "project.luau", manifest);
    td.write(dir / "main.luau", "return nil");
}

// Runs "/main.luau" from a MemorySource; returns "" or the error message.
std::string runMemory(std::unique_ptr<luau::MemorySource> source,
                      const std::vector<std::string> *allowed = nullptr,
                      const sonata::Pocket *pocket = nullptr) {
    luau::ModuleLoader loader(std::move(source));
    if (allowed) {
        loader.restrictLibraries(*allowed, "add it to build.libraries");
    }

    sonata::runtime::ProgramInfo info;
    info.name = "test";
    info.entry = "/main.luau";
    info.bundled = true;

    try {
        sonata::runtime::runProgram(loader, info, {}, pocket);
    } catch (const std::exception &e) {
        return e.what();
    }
    return "";
}

luau::Bytecode compile(std::string_view source) {
    return luau::Compiler().compile(source);
}

} // namespace

TEST_SUITE("build") {

TEST_CASE("DataFile reads --! directives from the top of a file") {
    using luau::DataFile;

    CHECK(DataFile::directives("--!strict\nreturn {}") == std::vector<std::string>{"strict"});
    CHECK(DataFile::directives("--[[ header ]]\n--!strict\n--!optimize 2\nreturn {}") ==
          std::vector<std::string>{"strict", "optimize"});
    CHECK(DataFile::directives("return {}\n--!strict").empty());
    CHECK(DataFile::directives("-- !strict\nreturn {}").empty());
}

TEST_CASE("Manifests carry build settings and --!strict, and keep them on save") {
    TempDir td;
    writeProject(td, "app", R"(--!strict
return {
    name = "app",
    build = {
        output = "bin/app",
        libraries = { "fs", "term" },
        assets = { "assets" },
        optimize = 2,
        debug = 0,
        compress = false,
        standalone = true,
    },
})");

    sonata::Project project = sonata::Project::open(td / "app");
    const auto &build = project.manifest().build;

    CHECK(project.manifest().strict);
    CHECK(build.output == "bin/app");
    REQUIRE(build.libraries);
    CHECK(*build.libraries == std::vector<std::string>{"fs", "term"});
    CHECK(build.assets == std::vector<std::string>{"assets"});
    CHECK(build.optimize == 2);
    CHECK(build.debug == 0);
    CHECK(build.compress == false);
    CHECK(build.standalone == true);

    project.addDependency({"dep", "https://example.invalid/dep.git", ""});

    const sonata::Project reopened = sonata::Project::open(td / "app");
    CHECK(reopened.manifest().strict);
    CHECK(reopened.manifest().build.libraries == build.libraries);
    CHECK(reopened.manifest().build.output == "bin/app");
}

TEST_CASE("Manifests without a build table get defaults; bad ones are rejected") {
    TempDir td;
    writeProject(td, "plain", R"(return { name = "plain" })");

    const sonata::Project plain = sonata::Project::open(td / "plain");
    CHECK_FALSE(plain.manifest().strict);
    CHECK_FALSE(plain.manifest().build.libraries);
    CHECK(plain.manifest().build.assets.empty());

    writeProject(td, "level", R"(return { name = "x", build = { optimize = 3 } })");
    CHECK_THROWS(sonata::Project::open(td / "level"));

    writeProject(td, "escape", R"(return { name = "x", build = { assets = { "../secret" } } })");
    CHECK_THROWS(sonata::Project::open(td / "escape"));

    writeProject(td, "libs", R"(return { name = "x", build = { libraries = "fs" } })");
    CHECK_THROWS(sonata::Project::open(td / "libs"));
}

TEST_CASE("Library selection unions the project and its dependencies") {
    TempDir td;
    writeProject(td, "app", R"(return { name = "app", build = { libraries = { "fs" } } })");
    writeProject(td, "app/.sonata/deps/a", R"(return { name = "a", build = { libraries = { "json", "fs" } } })");
    writeProject(td, "app/.sonata/deps/b", R"(return { name = "b" })");

    const sonata::Project app = sonata::Project::open(td / "app");
    const auto deps = sonata::openDependencies(app);
    REQUIRE(deps.size() == 2);

    SUBCASE("a dependency without a list means every library, with a warning") {
        const auto selection = sonata::selectLibraries(app, deps);
        CHECK(selection.all);
        CHECK(selection.libraries == sonata::lib::knownLibraryNames());
        REQUIRE(selection.warnings.size() == 1);
        CHECK(selection.warnings[0].find("'b'") != std::string::npos);
    }

    SUBCASE("with only declaring dependencies, the union is used") {
        const std::vector<sonata::InstalledProject> declaring = {deps[0]};
        const auto selection = sonata::selectLibraries(app, declaring);
        CHECK_FALSE(selection.all);
        CHECK(selection.libraries == std::vector<std::string>{"fs", "json"});
        CHECK(selection.warnings.empty());
    }

    SUBCASE("--libs replaces only the project's own list") {
        const std::vector<sonata::InstalledProject> declaring = {deps[0]};
        sonata::LibraryOptions options;
        options.rootLibraries = std::vector<std::string>{"term"};
        const auto selection = sonata::selectLibraries(app, declaring, options);
        CHECK(selection.libraries == std::vector<std::string>{"fs", "json", "term"});
    }

    SUBCASE("--strict turns the missing list into an error") {
        sonata::LibraryOptions options;
        options.strict = true;
        CHECK_THROWS_AS(sonata::selectLibraries(app, deps, options), sonata::BuildError);
    }
}

TEST_CASE("Library selection: --!strict manifests and unknown names are errors") {
    TempDir td;
    writeProject(td, "app", R"(return { name = "app", build = { libraries = { "fs" } } })");
    writeProject(td, "app/.sonata/deps/s", "--!strict\nreturn { name = \"s\" }");

    const sonata::Project app = sonata::Project::open(td / "app");
    CHECK_THROWS_WITH_AS(sonata::selectLibraries(app, sonata::openDependencies(app)),
                         doctest::Contains("--!strict"), sonata::BuildError);

    writeProject(td, "typo", R"(return { name = "typo", build = { libraries = { "jsno" } } })");
    const sonata::Project typo = sonata::Project::open(td / "typo");
    CHECK_THROWS_WITH_AS(sonata::selectLibraries(typo, {}), doctest::Contains("did you mean 'json'"),
                         sonata::BuildError);

    writeProject(td, "strictroot", "--!strict\nreturn { name = \"r\" }");
    const sonata::Project strictRoot = sonata::Project::open(td / "strictroot");
    CHECK_THROWS_AS(sonata::selectLibraries(strictRoot, {}), sonata::BuildError);

    sonata::LibraryOptions all;
    all.all = true;
    CHECK(sonata::selectLibraries(strictRoot, {}, all).all);
}

TEST_CASE("Restricted loaders refuse libraries that weren't selected") {
    auto source = std::make_unique<luau::MemorySource>();
    source->add("/main.luau", compile(R"(
        local path = require("@sonata/path")
        assert(path ~= nil)
        local ok, err = pcall(require, "@sonata/fs")
        assert(not ok)
        assert(string.find(err, "not enabled", 1, true), err)
        assert(string.find(err, "build.libraries", 1, true), err)
        return nil
    )"));

    const std::vector<std::string> allowed = {"path"};
    CHECK(runMemory(std::move(source), &allowed) == "");
}

TEST_CASE("Package entrypoints are resolved through the module source") {
    auto source = std::make_unique<luau::MemorySource>();
    source->add("/main.luau", compile(R"(
        local dep = require("@dep")
        assert(dep.name == "custom entry", dep.name)
        return nil
    )"));
    source->add("/.sonata/deps/dep/src/lib.luau", compile(R"(return { name = "custom entry" })"));
    source->addPackage("/", {"dep"});
    source->addPackage("/.sonata/deps/dep", {}, "src/lib.luau");

    CHECK(runMemory(std::move(source)) == "");
}

TEST_CASE("Bundles survive a round trip, compressed or not, and damage is detected") {
    TempDir td;
    const fs::path runtime = td.write("runtime", std::string(4096, '\x7f'));

    sonata::bundle::Bundle program;
    program.name = "demo";
    program.version = "1.0.0";
    program.entry = "/main.luau";
    program.libraries = {"path", "pocket"};
    program.libraryHint = "hint";
    program.aliases = {{"dep", "/.sonata/deps/dep"}};
    program.packages = {{"/", {"dep"}, "main.luau"}, {"/.sonata/deps/dep", {}, "init.luau"}};
    program.modules = {{"/main.luau", compile("return 1")}};
    program.assets = {{"/", "assets/a.bin", std::string("\0\1\2binary", 9)}};

    CHECK_FALSE(sonata::bundle::read(runtime));

    for (const bool compress : {true, false}) {
        CAPTURE(compress);
        const fs::path output = td / (compress ? "out-z" : "out-raw");
        sonata::bundle::write(runtime, output, program, compress);

        const auto back = sonata::bundle::read(output);
        REQUIRE(back);
        CHECK(back->name == "demo");
        CHECK(back->entry == "/main.luau");
        CHECK(back->libraries == program.libraries);
        CHECK(back->aliases == program.aliases);
        REQUIRE(back->packages.size() == 2);
        CHECK(back->packages[1].entrypoint == "init.luau");
        REQUIRE(back->modules.size() == 1);
        CHECK(back->modules[0].bytecode.bytes() == program.modules[0].bytecode.bytes());
        REQUIRE(back->assets.size() == 1);
        CHECK(back->assets[0].data == program.assets[0].data);

        // The runtime itself is untouched at the front of the file.
        CHECK(TempDir::read(output).compare(0, 4096, std::string(4096, '\x7f')) == 0);

        // A built program can't be used as a runtime.
        CHECK_THROWS_AS(sonata::bundle::write(output, td / "again", program, compress),
                        sonata::bundle::BundleError);
    }

    std::string damaged = TempDir::read(td / "out-z");
    damaged[4096 + 3] ^= 0x55;
    td.write("damaged", damaged);
    CHECK_THROWS_AS(sonata::bundle::read(td / "damaged"), sonata::bundle::BundleError);
}

TEST_CASE("Pocket paths are normalised and confined to the package") {
    using sonata::Pocket;
    CHECK(Pocket::normalize("assets/a.txt") == "assets/a.txt");
    CHECK(Pocket::normalize("./assets//b/../a.txt") == "assets/a.txt");
    CHECK(Pocket::normalize("assets\\a.txt") == "assets/a.txt");
    CHECK_FALSE(Pocket::normalize(""));
    CHECK_FALSE(Pocket::normalize("/etc/passwd"));
    CHECK_FALSE(Pocket::normalize("../outside"));
    CHECK_FALSE(Pocket::normalize("C:/x"));
    CHECK_FALSE(Pocket::normalize("."));
}

TEST_CASE("DiskPocket only serves declared assets") {
    TempDir td;
    td.write("pkg/assets/a.txt", "A");
    td.write("pkg/assets/sub/b.txt", "B");
    td.write("pkg/secret.txt", "S");
    td.write("pkg/assets/.sonata/x", "hidden");

    CHECK(sonata::collectAssets(td / "pkg", {"assets"}) ==
          std::vector<std::string>{"assets/a.txt", "assets/sub/b.txt"});
    CHECK_THROWS(sonata::collectAssets(td / "pkg", {"missing"}));

    sonata::DiskPocket pocket;
    pocket.addPackage("/pkg", td / "pkg", {"assets"});
    CHECK(pocket.read("/pkg", "assets/a.txt") == "A");
    CHECK(pocket.read("/pkg", "assets/sub/b.txt") == "B");
    CHECK_FALSE(pocket.read("/pkg", "secret.txt"));
    CHECK_FALSE(pocket.read("/pkg", "assets/.sonata/x"));
    CHECK(pocket.packageOf("/pkg/main.luau") == "/pkg");
}

TEST_CASE("@sonata/pocket reads the assets of the calling script's package") {
    auto source = std::make_unique<luau::MemorySource>();
    source->add("/main.luau", compile(R"(
        local pocket = require("@sonata/pocket")
        assert(pocket.bundled == true)
        assert(pocket.read("data/x.txt") == "root", "root asset")
        assert(pocket.exists("data/x.txt"))
        assert(not pocket.exists("nope"))
        assert(pocket.tryRead("nope") == nil)
        assert(#pocket.list() == 1)
        assert(#pocket.list("data") == 1)
        assert(not pcall(pocket.read, "nope"))
        assert(require("@dep").read() == "dep")
        return nil
    )"));
    source->add("/.sonata/deps/dep/init.luau", compile(R"(
        local pocket = require("@sonata/pocket")
        return { read = function() return pocket.read("data/x.txt") end }
    )"));
    source->addPackage("/", {"dep"});
    source->addPackage("/.sonata/deps/dep", {});

    sonata::MemoryPocket pocket;
    pocket.add("/", "data/x.txt", "root");
    pocket.add("/.sonata/deps/dep", "data/x.txt", "dep");

    CHECK(runMemory(std::move(source), nullptr, &pocket) == "");
}

}
