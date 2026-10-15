// tests/test_lsp.cpp
#include "helper.hpp"
#include <doctest/doctest.h>

#include <filesystem>
#include <sstream>
#include <string>

#include <sonata/core/library.hpp>
#include "lsp.hpp"

namespace fs = std::filesystem;

namespace {

void writeProject(const TempDir &td, const fs::path &dir, std::string_view manifest) {
    td.write(dir / ".sonata" / "project.luau", manifest);
    td.write(dir / "main.luau", "return nil");
}

// A package installed in <root>/.sonata/deps/<name>.
void writePackage(const TempDir &td, const std::string &name, std::string_view entrypoint) {
    const fs::path dir = fs::path(".sonata") / "deps" / name;
    td.write(dir / ".sonata" / "project.luau",
             "return { name = \"" + name + "\", entrypoint = \"" + std::string(entrypoint) + "\" }");
    td.write(dir / entrypoint, "return {}");
}

bool contains(const std::string &text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("lsp: setup writes the types, .luaurc and VS Code settings") {
    TempDir td;
    writeProject(td, ".", "return { name = \"app\" }");

    std::ostringstream out;
    sonata::lsp::setup(td.path(), {}, out);
    CHECK(out.str().empty());

    // A type file for every built-in library, plus the globals.
    for (const auto &name : sonata::lib::knownLibraryNames()) {
        INFO("library: " << name);
        CHECK(fs::is_regular_file(td / ".sonata" / "types" / "libs" / (name + ".luau")));
    }
    CHECK(fs::is_regular_file(td / ".sonata" / "types" / "globals.d.luau"));

    const std::string luaurc = TempDir::read(td / ".luaurc");
    CHECK(contains(luaurc, "\"sonata\": \".sonata/types/libs\""));

    const std::string settings = TempDir::read(td / ".vscode" / "settings.json");
    CHECK(contains(settings, "\"luau-lsp.platform.type\": \"standard\""));
    CHECK(contains(settings, "\"luau-lsp.sourcemap.enabled\": false"));
    CHECK(contains(settings, "\"@sonata\": \".sonata/types/globals.d.luau\""));

    CHECK(sonata::lsp::is_enabled(td.path()));
}

TEST_CASE("lsp: --no-vscode leaves .vscode alone") {
    TempDir td;
    writeProject(td, ".", "return { name = \"app\" }");

    std::ostringstream out;
    sonata::lsp::setup(td.path(), {.vscode = false}, out);
    CHECK(fs::exists(td / ".luaurc"));
    CHECK_FALSE(fs::exists(td / ".vscode"));
}

TEST_CASE("lsp: existing files are merged into") {
    TempDir td;
    writeProject(td, ".", "return { name = \"app\" }");
    td.write(".luaurc", R"({ "languageMode": "strict", "aliases": { "src": "./src", "SONATA": "elsewhere" } })");
    td.write(".vscode/settings.json", R"({
        "editor.tabSize": 2,
        "luau-lsp.platform.type": "roblox",
        "luau-lsp.types.definitionFiles": { "@extra": "extra.d.luau" },
    })");

    std::ostringstream out;
    sonata::lsp::setup(td.path(), {}, out);
    CHECK(out.str().empty());

    const std::string luaurc = TempDir::read(td / ".luaurc");
    CHECK(contains(luaurc, "\"languageMode\": \"strict\""));
    CHECK(contains(luaurc, "\"src\": \"./src\""));
    CHECK(contains(luaurc, "\"sonata\": \".sonata/types/libs\"")); // same name in another case is replaced
    CHECK_FALSE(contains(luaurc, "elsewhere"));

    const std::string settings = TempDir::read(td / ".vscode" / "settings.json");
    CHECK(contains(settings, "\"editor.tabSize\": 2"));
    CHECK(contains(settings, "\"luau-lsp.platform.type\": \"standard\""));
    CHECK(contains(settings, "\"@extra\": \"extra.d.luau\""));
    CHECK(contains(settings, "\"@sonata\": \".sonata/types/globals.d.luau\""));
}

TEST_CASE("lsp: files with comments are left alone, with a note") {
    TempDir td;
    writeProject(td, ".", "return { name = \"app\" }");
    const std::string original = "{\n    // mine\n    \"editor.tabSize\": 2\n}\n";
    td.write(".vscode/settings.json", original);

    std::ostringstream out;
    sonata::lsp::setup(td.path(), {}, out);

    CHECK(TempDir::read(td / ".vscode" / "settings.json") == original);
    CHECK(contains(out.str(), "has comments"));
    CHECK(contains(out.str(), "\"luau-lsp.platform.type\": \"standard\""));
}

TEST_CASE("lsp: an invalid settings.json is reported, not overwritten") {
    TempDir td;
    writeProject(td, ".", "return { name = \"app\" }");
    td.write(".vscode/settings.json", "{ \"a\": ");

    std::ostringstream out;
    sonata::lsp::setup(td.path(), {}, out);

    CHECK(TempDir::read(td / ".vscode" / "settings.json") == "{ \"a\": ");
    CHECK(contains(out.str(), "could not parse"));
}

TEST_CASE("lsp: package aliases follow .sonata/deps") {
    TempDir td;
    writeProject(td, ".", "return { name = \"app\" }");
    writePackage(td, "plain", "init.luau");
    writePackage(td, "app_lib", "main.luau");
    writePackage(td, "nested", "src/init.luau");

    std::ostringstream out;
    sonata::lsp::setup(td.path(), {}, out);

    std::string luaurc = TempDir::read(td / ".luaurc");
    CHECK(contains(luaurc, "\"plain\": \".sonata/deps/plain\""));
    CHECK(contains(luaurc, "\"app_lib\": \".sonata/deps/app_lib/main\""));
    CHECK(contains(luaurc, "\"nested\": \".sonata/deps/nested/src\""));

    // A package goes away, another arrives.
    fs::remove_all(td / ".sonata" / "deps" / "plain");
    writePackage(td, "fresh", "init.luau");
    sonata::lsp::refresh(td.path(), out);

    luaurc = TempDir::read(td / ".luaurc");
    CHECK_FALSE(contains(luaurc, "plain"));
    CHECK(contains(luaurc, "\"fresh\": \".sonata/deps/fresh\""));
    CHECK(contains(luaurc, "\"app_lib\": \".sonata/deps/app_lib/main\""));
    CHECK(out.str().empty());
}

TEST_CASE("lsp: refresh does nothing for projects without LSP support") {
    TempDir td;
    writeProject(td, ".", "return { name = \"app\" }");
    writePackage(td, "plain", "init.luau");

    CHECK_FALSE(sonata::lsp::is_enabled(td.path()));

    std::ostringstream err;
    sonata::lsp::refresh(td.path(), err);
    CHECK_FALSE(fs::exists(td / ".luaurc"));
    CHECK(err.str().empty());

    // A .luaurc of the user's own isn't taken over either.
    td.write(".luaurc", R"({ "aliases": { "x": "./x" } })");
    sonata::lsp::refresh(td.path(), err);
    CHECK(TempDir::read(td / ".luaurc") == R"({ "aliases": { "x": "./x" } })");
}
