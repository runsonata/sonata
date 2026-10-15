#pragma once

#include <filesystem>
#include <ostream>

/*
 * Luau Language Server (luau-lsp) support for Sonata projects.
 *
 *   .sonata/types/libs/<name>.luau   types of the @sonata/<name> libraries
 *   .sonata/types/globals.d.luau     types of the globals Sonata adds (args, sonata)
 *   .luaurc                          aliases: "sonata" -> .sonata/types/libs, and
 *                                    one per package in .sonata/deps
 *   .vscode/settings.json            luau-lsp settings for VS Code
 *
 * The type files come from types/ in the source tree and are compiled into sn
 * (see cli/CMakeLists.txt). .luaurc and settings.json are merged into, so
 * anything else they hold is kept.
 */
namespace sonata::lsp {

struct SetupOptions {
    /* Also write .vscode/settings.json. */
    bool vscode = true;
};

/*
 * Writes (or rewrites) every file above for the project at "root". Notes for
 * the user (such as settings that have to be added by hand) go to "out".
 * Throws std::runtime_error.
 */
void setup(const std::filesystem::path &root, const SetupOptions &options,
           std::ostream &out);

/* True if "root" was set up by setup(): its .luaurc maps @sonata to the types. */
bool is_enabled(const std::filesystem::path &root);

/*
 * Points one .luaurc alias at every package in .sonata/deps, and removes the
 * aliases of packages that are gone. Throws std::runtime_error.
 */
void sync_aliases(const std::filesystem::path &root);

/*
 * sync_aliases() for projects where is_enabled(), for commands that change
 * .sonata/deps. A failure is only a warning, written to "err".
 */
void refresh(const std::filesystem::path &root, std::ostream &err);

} // namespace sonata::lsp
