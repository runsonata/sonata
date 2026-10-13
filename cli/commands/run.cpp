#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sonata/core/package.hpp>
#include <sonata/core/pocket.hpp>
#include <sonata/core/project.hpp>
#include <sonata/core/module.hpp>
#include <sonata/core/runtime.hpp>
#include <sonata/core/selection.hpp>

#include "commands/run.hpp"

namespace fs = std::filesystem;

namespace sonata::commands {

/*
 * HOW "sn run" WORKS
 *
 *   1. Project         Project::find() locates .sonata/project.luau (which
 *                      reads it through DataFile) and the entrypoint.
 *   2. PackageManager  Makes sure the declared dependencies are installed
 *                      (--install fetches them, otherwise it's an error) and
 *                      registers an "@name" alias for each one.
 *   3. Libraries       selectLibraries() works out which built-ins the
 *                      project (and its dependencies) declared in
 *                      build.libraries; the rest are refused, exactly as a
 *                      program made by "sn build" wouldn't have them.
 *   4. ModuleLoader    Serves require() from disk. FileSystemSource compiles
 *                      .luau files on demand with the Compiler.
 *   5. runProgram()    Creates the VM with the "sonata" and "args" globals,
 *                      installs require() and @sonata/pocket (reading the
 *                      declared build.assets from disk), sandboxes the
 *                      globals and runs the entrypoint. Built programs go
 *                      through the same function (see runtime/main.cpp).
 */

namespace {

// argv[0] is the command itself ("run"); its arguments start after it.
constexpr int kFirstArgument = 0;

struct Options {
    fs::path start = ".";          // project directory, or any file inside it
    std::optional<fs::path> entry; // --entry, relative to the project root
    bool install = false;
    bool verbose = false;
    std::vector<std::string> scriptArgs; // everything after "--"
};

int fail(const std::string &message) {
    std::cerr << "error: " << message << '\n';
    return 1;
}

// Returns std::nullopt (after printing why) if the command line is invalid.
std::optional<Options> parseOptions(int argc, char **argv) {
    constexpr std::string_view entryPrefix = "--entry=";

    auto reject = [](const std::string &message) -> std::optional<Options> {
        fail(message);
        return std::nullopt;
    };

    Options options;
    bool haveStart = false;

    for (int i = kFirstArgument; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--") {
            for (++i; i < argc; ++i) {
                options.scriptArgs.emplace_back(argv[i]);
            }
            break;
        }

        if (arg == "--install" || arg == "-i") {
            options.install = true;
        } else if (arg == "--verbose" || arg == "-v") {
            options.verbose = true;
        } else if (arg == "--entry" || arg == "-e") {
            if (i + 1 >= argc) {
                return reject("--entry needs a file");
            }
            options.entry = fs::path(argv[++i]);
        } else if (arg.starts_with(entryPrefix)) {
            options.entry = fs::path(std::string(arg.substr(entryPrefix.size())));
        } else if (arg.size() > 1 && arg.front() == '-') {
            return reject("unknown option '" + std::string(arg) +
                          "' (use -- to pass options to the script)");
        } else if (haveStart) {
            return reject("unexpected argument '" + std::string(arg) +
                          "' (use -- to pass arguments to the script)");
        } else {
            options.start = fs::path(std::string(arg));
            haveStart = true;
        }
    }

    if (options.entry && options.entry->empty()) {
        return reject("--entry needs a file");
    }

    return options;
}

/*
 * The module path ModuleLoader::run() wants: absolute, forward slashes,
 * normalised. Defaults to the manifest's entrypoint.
 */
std::string resolveEntry(const Project &project,
                         const std::optional<fs::path> &entryOverride) {
    fs::path file = project.entrypoint();

    if (entryOverride) {
        file = project.root() / *entryOverride;
        if (!fs::is_regular_file(file)) {
            throw std::runtime_error("entry file not found: " +
                                     file.generic_string());
        }
    }

    auto path = luau::normalizePath(fs::absolute(file).generic_string());
    if (!path) {
        throw std::runtime_error("can't use '" + file.generic_string() +
                                 "' as a module path");
    }
    return *path;
}

/*
 * With --install, installs whatever is missing (install() leaves anything that
 * is already present and still matching the manifest alone). Without it,
 * running with missing dependencies is an error rather than a surprise
 * download.
 */
void prepareDependencies(PackageManager &packages, const Project &project,
                         bool install) {
    if (install) {
        packages.setProgressCallback(
            [](const std::string &line) { std::cerr << line << '\n'; });
        packages.install();
        return;
    }

    std::string missing;
    for (const auto &dependency : project.manifest().dependencies) {
        if (packages.isInstalled(dependency.name)) {
            continue;
        }
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += dependency.name;
    }

    if (!missing.empty()) {
        throw PackageError("missing dependencies: " + missing +
                           " (pass --install to fetch them)");
    }
}

// A folder on disk as a module path (see module.hpp).
std::string modulePath(const fs::path &directory) {
    auto path = luau::normalizePath(fs::absolute(directory).generic_string());
    if (!path) {
        throw std::runtime_error("can't use '" + directory.generic_string() +
                                 "' as a module path");
    }
    return *path;
}

void printSummary(const Project &project, const std::string &entry,
                  const PackageManager *packages,
                  const LibrarySelection &libraries) {
    const Project::Manifest &manifest = project.manifest();

    std::string names;
    if (packages != nullptr) {
        for (const auto &package : packages->installed()) {
            if (!names.empty()) {
                names += ", ";
            }
            names += package.dependency.name;
        }
    }

    std::cerr << "project   " << manifest.name << ' ' << manifest.version
              << " (" << project.root().generic_string() << ")\n"
              << "entry     " << entry << '\n'
              << "packages  " << (names.empty() ? "none" : names) << '\n';

    std::string libs = "all";
    if (!libraries.all) {
        libs.clear();
        for (const std::string &name : libraries.libraries) {
            libs += (libs.empty() ? "" : ", ") + name;
        }
    }
    std::cerr << "libraries " << (libs.empty() ? "none" : libs) << '\n';

    for (const std::string &warning : libraries.warnings) {
        std::cerr << "warning: " << warning << '\n';
    }
}

} // namespace

int run(int argc, char **argv) {
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options) {
        return 1;
    }

    try {
        // 1. The project.
        if (!fs::exists(options->start)) {
            return fail("path does not exist: " + options->start.string());
        }

        Project project = Project::find(fs::canonical(options->start));
        const std::string entry = resolveEntry(project, options->entry);

        // 2. Dependencies. Skipped entirely for projects that have none.
        std::optional<PackageManager> packages;
        if (options->install || !project.manifest().dependencies.empty() ||
            project.hasDependenciesDirectory()) {
            packages.emplace(project);
            prepareDependencies(*packages, project, options->install);
        }

        // 3. Which built-in libraries the program may use. Throws BuildError
        //    for an unknown name or a --!strict manifest without a list.
        const std::vector<InstalledProject> dependencies =
            openDependencies(project);
        const LibrarySelection libraries =
            selectLibraries(project, dependencies);

        if (options->verbose) {
            printSummary(project, entry, packages ? &*packages : nullptr,
                         libraries);
        }

        // Every package's build.assets, for @sonata/pocket. Declared before
        // the loader so it outlives the VM.
        DiskPocket pocket;
        pocket.addPackage(modulePath(project.root()), project.root(),
                          project.manifest().build.assets);
        for (const InstalledProject &dependency : dependencies) {
            pocket.addPackage(modulePath(dependency.project.root()),
                              dependency.project.root(),
                              dependency.project.manifest().build.assets);
        }

        // 4. The module system. The loader must outlive the VM (runProgram
        //    creates it) and must not move.
        luau::ModuleLoader loader(std::make_unique<luau::FileSystemSource>());
        if (packages) {
            packages->registerAliases(loader);
        }
        if (!libraries.all) {
            loader.restrictLibraries(libraries.libraries, libraries.hint);
        }

        // 5. Go. Throws ModuleError (with a stack trace) if the script fails.
        runtime::ProgramInfo program;
        program.name = project.manifest().name;
        program.version = project.manifest().version;
        program.root = project.root().generic_string();
        program.entry = entry;

        runtime::runProgram(loader, program, options->scriptArgs, &pocket);
        return 0;
    } catch (const luau::ModuleError &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        return fail(error.what());
    }
}

const cli::Command &run_command() {
    static const cli::Argument entry_arg{
        .name = "--entry",
        .aliases = {"-e"},
        .description = "Run <file> (relative to the project root) instead of "
                       "the manifest's entrypoint"};
    static const cli::Argument verbose_arg{
        .name = "--verbose",
        .aliases = {"-v"},
        .description = "Print the project, entry, packages and libraries "
                       "being used, and any warnings about them"};

    static const cli::Command command{
        .name = "run",
        .description = "Run a project without compiling it (aka live test)",
        .args = {cli::help_argument, entry_arg, verbose_arg},
        .usage = {"sn run [path] [options] [-- <args>...]"},
        .examples = {"sn run",
                     "sn run ./my-project",
                     "sn run --entry tools/seed.luau",
                     "sn run -- --port 8080"},

        .execute = run,

        .children = {}

    };

    return command;
}

} // namespace sonata::commands