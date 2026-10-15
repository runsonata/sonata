#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "Luau/Ast.h"
#include "Luau/Parser.h"

#include <sonata/core/bundle.hpp>
#include <sonata/core/info.hpp>
#include <sonata/core/library.hpp>
#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/module.hpp>
#include <sonata/core/package.hpp>
#include <sonata/core/pocket.hpp>
#include <sonata/core/project.hpp>
#include <sonata/core/selection.hpp>
#include <sonata/core/system.hpp>

#include "commands/build.hpp"
#include "lsp.hpp"

namespace fs = std::filesystem;

namespace sonata::commands {

/*
 * HOW "sn build" WORKS
 *
 *   1. Settings      build = { ... } from .sonata/project.luau, overridden by
 *                    command-line flags for this one build.
 *   2. Libraries     selectLibraries() (selection.hpp) combines the
 *                    build.libraries of the project and every dependency.
 *   3. Compile       Every .luau file of the project and of each dependency is
 *                    compiled to bytecode under a virtual root ("/main.luau",
 *                    "/.sonata/deps/foo/init.luau", ...), and every literal
 *                    require("@sonata/x") is checked against step 2.
 *   4. Assets        build.assets of every package, for @sonata/pocket.
 *   5. Runtime       The prebuilt runtime (snrt, installed next to sn) has
 *                    every library. When fewer are selected, or --standalone
 *                    is asked for, a runtime with only those is built from
 *                    Sonata's source with CMake and cached; without a
 *                    toolchain the prebuilt one is used instead (the program
 *                    still can't require the other libraries, it's just
 *                    bigger).
 *   6. Write         Runtime + bundle (bundle.hpp) -> one executable.
 */

namespace {

using luau::Compiler;

constexpr std::string_view kSourceRepository = "https://github.com/sonata-cli/sonata.git";
constexpr std::string_view kDepsRoot = "/.sonata/deps/";

#ifdef _WIN32
constexpr std::string_view kExeSuffix = ".exe";
#else
constexpr std::string_view kExeSuffix = "";
#endif

int fail(const std::string &message) {
    std::cerr << "error: " << message << '\n';
    return 1;
}

void warn(const std::string &message) {
    std::cerr << "warning: " << message << '\n';
}

// Options ///////////////////////////////////////////////////////////////////

struct Options {
    fs::path start = ".";
    std::optional<fs::path> output;
    std::optional<fs::path> entry;
    std::optional<std::vector<std::string>> libraries;
    std::optional<int> optimize;
    std::optional<int> debug;
    std::optional<bool> compress;
    std::optional<fs::path> runtime;
    bool allLibraries = false;
    bool standalone = false;
    bool strict = false;
    bool install = false;
    bool verbose = false;
};

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> items;
    std::size_t pos = 0;

    while (pos <= text.size()) {
        std::size_t next = text.find(',', pos);
        if (next == std::string_view::npos) {
            next = text.size();
        }

        std::string item(text.substr(pos, next - pos));
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) {
            items.push_back(std::move(item));
        }

        pos = next + 1;
    }

    return items;
}

std::optional<int> parseLevel(std::string_view text) {
    if (text == "0" || text == "1" || text == "2") {
        return text[0] - '0';
    }
    return std::nullopt;
}

// Returns std::nullopt (after printing why) if the command line is invalid.
std::optional<Options> parseOptions(int argc, char **argv) {
    Options options;
    bool haveStart = false;

    auto reject = [](const std::string &message) -> std::optional<Options> {
        fail(message);
        return std::nullopt;
    };

    for (int i = 0; i < argc; ++i) {
        const std::string_view arg = argv[i];

        // "--name=value" and "--name value" are both accepted.
        std::string_view name = arg;
        std::optional<std::string_view> inlineValue;
        if (arg.starts_with("--")) {
            if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
                name = arg.substr(0, eq);
                inlineValue = arg.substr(eq + 1);
            }
        }

        auto value = [&](std::string_view what) -> std::optional<std::string_view> {
            if (inlineValue) {
                return inlineValue;
            }
            if (i + 1 >= argc) {
                fail(std::string(name) + " needs " + std::string(what));
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };

        auto level = [&](std::optional<int> &target) -> bool {
            const auto text = value("a level (0, 1 or 2)");
            if (!text) {
                return false;
            }
            target = parseLevel(*text);
            if (!target) {
                fail(std::string(name) + " must be 0, 1 or 2");
                return false;
            }
            return true;
        };

        if (name == "--output" || name == "-o") {
            const auto text = value("a file");
            if (!text || text->empty()) {
                return reject("--output needs a file");
            }
            options.output = fs::path(std::string(*text));
        } else if (name == "--entry" || name == "-e") {
            const auto text = value("a file");
            if (!text || text->empty()) {
                return reject("--entry needs a file");
            }
            options.entry = fs::path(std::string(*text));
        } else if (name == "--libs" || name == "-l") {
            const auto text = value("a comma-separated list of libraries");
            if (!text) {
                return std::nullopt;
            }
            options.libraries = splitList(*text);
        } else if (name == "--all-libs") {
            options.allLibraries = true;
        } else if (name == "--optimize" || name == "-O") {
            if (!level(options.optimize)) {
                return std::nullopt;
            }
        } else if (name == "--debug-level" || name == "-g") {
            if (!level(options.debug)) {
                return std::nullopt;
            }
        } else if (name == "--compress") {
            options.compress = true;
        } else if (name == "--no-compress") {
            options.compress = false;
        } else if (name == "--standalone") {
            options.standalone = true;
        } else if (name == "--strict") {
            options.strict = true;
        } else if (name == "--runtime") {
            const auto text = value("a file");
            if (!text || text->empty()) {
                return reject("--runtime needs a file");
            }
            options.runtime = fs::path(std::string(*text));
        } else if (name == "--install" || name == "-i") {
            options.install = true;
        } else if (name == "--verbose" || name == "-v") {
            options.verbose = true;
        } else if (arg.size() > 1 && arg.front() == '-') {
            return reject("unknown option '" + std::string(arg) + "'");
        } else if (haveStart) {
            return reject("unexpected argument '" + std::string(arg) + "'");
        } else {
            options.start = fs::path(std::string(arg));
            haveStart = true;
        }
    }

    if (options.libraries && options.allLibraries) {
        return reject("--libs and --all-libs can't be used together");
    }

    return options;
}

// Settings: manifest + command line /////////////////////////////////////////

struct Settings {
    fs::path output;
    Compiler::Options compiler;
    bool compress = true;
    bool standalone = false;
};

// Manifest names may contain anything; file names shouldn't.
std::string fileNameFor(const std::string &name) {
    std::string result;
    for (const char c : name) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        result += safe ? c : '_';
    }
    return result.empty() || result == "." || result == ".." ? "program" : result;
}

fs::path withExeSuffix(fs::path path) {
    if (!kExeSuffix.empty() && path.extension() != kExeSuffix) {
        path += std::string(kExeSuffix);
    }
    return path;
}

Settings resolveSettings(const Project &project, const Options &options) {
    const Project::Build &build = project.manifest().build;
    Settings settings;

    if (options.output) {
        settings.output = fs::absolute(*options.output); // relative to where sn was run
    } else if (build.output) {
        settings.output = project.root() / fs::path(*build.output);
    } else {
        settings.output = project.root() / "dist" / fileNameFor(project.manifest().name);
    }
    settings.output = withExeSuffix(settings.output.lexically_normal());

    settings.compiler.optimizationLevel = options.optimize.value_or(build.optimize.value_or(1));
    settings.compiler.debugLevel = options.debug.value_or(build.debug.value_or(1));
    settings.compress = options.compress.value_or(build.compress.value_or(true));
    settings.standalone = options.standalone || build.standalone.value_or(false);

    return settings;
}

// Dependencies (same rules as "sn run") /////////////////////////////////////

void prepareDependencies(Project &project, bool install) {
    if (!install && project.manifest().dependencies.empty()) {
        return;
    }

    PackageManager packages(project);

    if (install) {
        packages.setProgressCallback([](const std::string &line) { std::cerr << line << '\n'; });
        packages.install();
        lsp::refresh(project.root(), std::cerr);
        return;
    }

    std::string missing;
    for (const auto &dependency : project.manifest().dependencies) {
        if (!packages.isInstalled(dependency.name)) {
            missing += (missing.empty() ? "" : ", ") + dependency.name;
        }
    }

    if (!missing.empty()) {
        throw PackageError("missing dependencies: " + missing + " (pass --install to fetch them)");
    }
}

// Compiling /////////////////////////////////////////////////////////////////

// One package to compile: the project ("/") or a dependency.
struct Unit {
    std::string name;    // "" for the project itself
    std::string root;    // virtual root, "/" or "/.sonata/deps/<name>"
    const Project *project;
};

std::string virtualPath(const Unit &unit, const fs::path &file) {
    const std::string relative = file.lexically_relative(unit.project->root()).generic_string();
    return unit.root == "/" ? "/" + relative : unit.root + "/" + relative;
}

std::string readSource(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw BuildError("could not read " + file.generic_string());
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

// Every literal require("@sonata/<name>") in a chunk, with its line.
struct BuiltinRequire {
    std::string library;
    int line;
};

class RequireFinder : public Luau::AstVisitor {
public:
    std::vector<BuiltinRequire> found;

    bool visit(Luau::AstExprCall *call) override {
        auto *function = call->func->as<Luau::AstExprGlobal>();
        if (function == nullptr || std::strcmp(function->name.value, "require") != 0 || call->args.size < 1) {
            return true;
        }

        auto *argument = call->args.data[0]->as<Luau::AstExprConstantString>();
        if (argument == nullptr) {
            return true;
        }

        const std::string_view request(argument->value.data, argument->value.size);
        constexpr std::string_view prefix = "@sonata/";
        if (request.starts_with(prefix) && request.size() > prefix.size()) {
            found.push_back({std::string(request.substr(prefix.size())),
                             static_cast<int>(call->location.begin.line) + 1});
        }
        return true;
    }
};

std::vector<BuiltinRequire> findBuiltinRequires(const std::string &source) {
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions options;

    Luau::ParseResult result = Luau::Parser::parse(source.data(), source.size(), names, allocator, options);

    RequireFinder finder;
    if (result.root != nullptr) {
        result.root->visit(&finder);
    }
    return finder.found;
}

struct CompiledProgram {
    std::vector<bundle::Module> modules;
    std::vector<std::string> problems; // compile errors and unselected libraries
};

CompiledProgram compileUnits(const std::vector<Unit> &units, const Compiler &compiler,
                             const LibrarySelection &libraries) {
    CompiledProgram program;

    for (const Unit &unit : units) {
        for (const fs::path &file : unit.project->sourceFiles()) {
            const std::string path = virtualPath(unit, file);
            const std::string shown = file.lexically_relative(units.front().project->root()).generic_string();
            const std::string source = readSource(file);

            luau::Bytecode bytecode = compiler.compile(source);
            if (const auto error = bytecode.compileError()) {
                program.problems.push_back(shown + *error);
                continue;
            }

            for (const BuiltinRequire &require : findBuiltinRequires(source)) {
                const bool selected = std::binary_search(libraries.libraries.begin(),
                                                         libraries.libraries.end(), require.library);
                if (selected) {
                    continue;
                }

                const auto &known = lib::knownLibraryNames();
                const bool exists = std::binary_search(known.begin(), known.end(), require.library);

                program.problems.push_back(
                    shown + ":" + std::to_string(require.line) + ": requires '@sonata/" + require.library + "', " +
                    (exists ? "which isn't in build.libraries" +
                                  (unit.name.empty() ? std::string() : " of dependency '" + unit.name + "'")
                            : "which is not a built-in library"));
            }

            program.modules.push_back({path, std::move(bytecode)});
        }
    }

    return program;
}

// Runtimes //////////////////////////////////////////////////////////////////

std::string listOf(const std::vector<std::string> &names) {
    std::string text;
    for (const std::string &name : names) {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text.empty() ? "none" : text;
}

std::string humanSize(std::uintmax_t bytes) {
    std::ostringstream out;
    if (bytes >= 1024 * 1024) {
        out << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
    } else if (bytes >= 1024) {
        out << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / 1024.0 << " KiB";
    } else {
        out << bytes << " B";
    }
    return out.str();
}

std::optional<fs::path> envPath(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return fs::path(value);
}

bool isFile(const fs::path &path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// The libraries compiled into this sn, and so into the runtime built with it.
std::vector<std::string> compiledLibraries() {
    lib::LibraryRegistry registry;
    lib::registerBuiltinLibraries(registry);
    return registry.names();
}

// The runtime installed with sn: $SONATA_RUNTIME, next to sn, in
// ../libexec/sonata (an installed copy), or ../runtime (a build tree).
std::optional<fs::path> findPrebuiltRuntime() {
    if (auto path = envPath("SONATA_RUNTIME")) {
        return isFile(*path) ? std::optional<fs::path>(*path) : std::nullopt;
    }

    const std::optional<fs::path> self = os::executablePath();
    if (!self) {
        return std::nullopt;
    }

    const std::string file = "snrt" + std::string(kExeSuffix);
    const fs::path directory = self->parent_path();

    for (const fs::path &candidate : {directory / file, directory / ".." / "libexec" / "sonata" / file,
                                      directory / ".." / "runtime" / file}) {
        if (isFile(candidate)) {
            return candidate.lexically_normal();
        }
    }

    return std::nullopt;
}

// $SONATA_HOME, or ~/.sonata.
fs::path sonataHome() {
    if (auto home = envPath("SONATA_HOME")) {
        return *home;
    }
    if (auto home = os::homeDirectory()) {
        return *home / ".sonata";
    }
    throw BuildError("can't find a home directory to cache runtimes in (set SONATA_HOME)");
}

std::string hashOf(const std::string &text) {
    std::uint64_t hash = 14695981039346656037ull; // FNV-1a
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

bool isSourceTree(const fs::path &path) {
    return isFile(path / "CMakeLists.txt") && isFile(path / "runtime" / "main.cpp") &&
           isFile(path / "external" / "luau" / "CMakeLists.txt");
}

// Where Sonata's source comes from for a tailored runtime: $SONATA_SOURCE, the
// tree this sn was built from (if it's still there), or a cached clone of
// this version's release tag.
fs::path findSource(bool verbose) {
    if (auto path = envPath("SONATA_SOURCE")) {
        if (!isSourceTree(*path)) {
            throw BuildError("SONATA_SOURCE (" + path->generic_string() +
                             ") is not a Sonata source tree with its submodules checked out");
        }
        return *path;
    }

#ifdef SONATA_SOURCE_DIR
    if (isSourceTree(SONATA_SOURCE_DIR)) {
        return fs::path(SONATA_SOURCE_DIR);
    }
#endif

    const std::string tag = "v" + info::version_string();
    const fs::path clone = sonataHome() / "source" / tag;
    if (isSourceTree(clone)) {
        return clone;
    }

    std::cerr << "fetching Sonata " << tag << " source from " << kSourceRepository << " ...\n";

    std::error_code ec;
    fs::remove_all(clone, ec);
    fs::create_directories(clone.parent_path(), ec);

    const os::ProcessResult result = os::runProcess(
        {"git", "clone", "--quiet", "--depth", "1", "--branch", tag, "--recurse-submodules", "--shallow-submodules",
         "--", std::string(kSourceRepository), clone.string()},
        {{"GIT_TERMINAL_PROMPT", "0"}}, verbose);

    if (!result.started) {
        throw BuildError("could not run git to fetch Sonata's source (set SONATA_SOURCE to a checkout instead)");
    }
    if (result.exitCode != 0 || !isSourceTree(clone)) {
        fs::remove_all(clone, ec);
        throw BuildError("could not fetch Sonata " + tag + " from " + std::string(kSourceRepository) +
                         (result.output.empty() ? "" : ":\n" + result.output) +
                         "\n(set SONATA_SOURCE to a checkout of Sonata's source instead)");
    }

    return clone;
}

std::string cmakeList(const std::vector<std::string> &names) {
    std::string text;
    for (const std::string &name : names) {
        text += (text.empty() ? "" : ";") + name;
    }
    return text;
}

void runCMake(const std::vector<std::string> &args, const char *what, bool verbose) {
    const os::ProcessResult result = os::runProcess(args, {}, verbose);

    if (!result.started) {
        throw BuildError("could not run cmake (is it installed and on PATH?)");
    }
    if (result.exitCode != 0) {
        std::string message = std::string(what) + " failed";
        if (!result.output.empty()) {
            message += ":\n" + result.output;
        }
        throw BuildError(message);
    }
}

// Builds (or reuses) a runtime with only "libraries" compiled in. The CMake
// build directory is kept, so later builds are incremental, and a changed
// source tree is picked up automatically.
fs::path buildTailoredRuntime(const std::vector<std::string> &libraries, bool standalone, bool verbose) {
    const fs::path source = findSource(verbose);

    const std::string key = info::version_string() + "|" + std::string(info::git_hash) + "|" +
                            info::platform_string() + "|" + cmakeList(libraries) + "|" +
                            (standalone ? "standalone" : "default") + "|" + fs::absolute(source).generic_string();

    const fs::path directory = sonataHome() / "runtimes" /
                               (info::version_string() + "-" + info::platform_string() + "-" + hashOf(key));
    const fs::path buildDir = directory / "build";
    const fs::path configured = buildDir / ".sonata-configured";
    const fs::path runtime = buildDir / "runtime" / ("snrt" + std::string(kExeSuffix));

    std::cerr << "building a runtime with " << (standalone ? "no shared libraries and " : "")
              << "only: " << listOf(libraries)
              << (isFile(runtime) ? "\n" : " (the first build of a combination takes a few minutes)\n");

    if (!isFile(configured)) {
        std::vector<std::string> args = {
            "cmake",
            "-S", source.string(),
            "-B", buildDir.string(),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DSONATA_BUILD_CLI=OFF",
            "-DSONATA_BUILD_TESTS=OFF",
            "-DSONATA_INSTALL=OFF",
            "-DSONATA_BUILD_RUNTIME=ON",
            "-DSONATA_LIBS=" + cmakeList(libraries),
            std::string("-DSONATA_STANDALONE=") + (standalone ? "ON" : "OFF"),
        };
        runCMake(args, "configuring the runtime", verbose);
        std::ofstream(configured) << key << '\n';
    }

    runCMake({"cmake", "--build", buildDir.string(), "--target", "snrt", "--config", "Release", "--parallel"},
             "building the runtime", verbose);

    if (!isFile(runtime)) {
        throw BuildError("the runtime build finished, but " + runtime.generic_string() + " is missing");
    }

    return runtime;
}

struct ChosenRuntime {
    fs::path path;
    std::string description;
};

ChosenRuntime chooseRuntime(const Options &options, const Settings &settings, const LibrarySelection &selection) {
    if (options.runtime) {
        if (!isFile(*options.runtime)) {
            throw BuildError("--runtime: " + options.runtime->generic_string() + " does not exist");
        }
        return {*options.runtime, "custom (" + options.runtime->generic_string() + ")"};
    }

    // The prebuilt runtime has the same libraries as this sn.
    const std::vector<std::string> available = compiledLibraries();
    const bool prebuiltCovers = std::includes(available.begin(), available.end(), selection.libraries.begin(),
                                              selection.libraries.end());
    const bool needTailored = settings.standalone || selection.libraries != available;

    if (!needTailored) {
        if (auto prebuilt = findPrebuiltRuntime()) {
            return {*prebuilt, "prebuilt, every library"};
        }
    }

    try {
        const fs::path tailored = buildTailoredRuntime(selection.libraries, settings.standalone, options.verbose);
        return {tailored, std::string(settings.standalone ? "standalone, " : "tailored, ") +
                              (selection.all ? "every library" : listOf(selection.libraries))};
    } catch (const BuildError &error) {
        // --standalone is a promise the prebuilt runtime can't keep.
        if (settings.standalone) {
            throw BuildError(std::string(error.what()) +
                             "\n(--standalone needs a runtime built from source: CMake, a C++ compiler and the "
                             "static libraries)");
        }

        const std::optional<fs::path> prebuilt = prebuiltCovers ? findPrebuiltRuntime() : std::nullopt;
        if (!prebuilt) {
            throw;
        }

        warn(std::string(error.what()));
        warn("couldn't build a runtime with only the selected libraries, so the prebuilt one is used: the "
             "program can still only require the selected libraries, but it is larger than it needs to be");
        return {*prebuilt, "prebuilt, restricted to " + listOf(selection.libraries)};
    }
}

// The command ///////////////////////////////////////////////////////////////

std::string resolveEntry(const Project &project, const std::optional<fs::path> &entryOverride) {
    fs::path file = project.entrypoint();

    if (entryOverride) {
        file = (project.root() / *entryOverride).lexically_normal();
        if (!isFile(file)) {
            throw BuildError("entry file not found: " + file.generic_string());
        }
    }

    const std::string relative = file.lexically_relative(project.root()).generic_string();
    if (relative.empty() || relative.starts_with("..") || relative.starts_with(".sonata/") ||
        file.extension() != ".luau") {
        throw BuildError("the entry must be a .luau file inside the project: " + file.generic_string());
    }

    return "/" + relative;
}

void addAssets(bundle::Bundle &program, const std::string &package, const Project &project) {
    try {
        for (const std::string &path : collectAssets(project.root(), project.manifest().build.assets)) {
            program.assets.push_back({package, path, readSource(project.root() / fs::path(path))});
        }
    } catch (const std::runtime_error &error) {
        throw BuildError(project.projectFile().generic_string() + ": " + error.what());
    }
}

int runBuild(const Options &options) {
    if (!fs::exists(options.start)) {
        return fail("path does not exist: " + options.start.string());
    }

    Project project = Project::find(fs::canonical(options.start));
    prepareDependencies(project, options.install);

    const Settings settings = resolveSettings(project, options);
    const std::string entry = resolveEntry(project, options.entry);

    // Libraries.
    const std::vector<InstalledProject> dependencies = openDependencies(project);

    LibraryOptions libraryOptions;
    libraryOptions.rootLibraries = options.libraries;
    libraryOptions.all = options.allLibraries;
    libraryOptions.strict = options.strict;
    const LibrarySelection selection = selectLibraries(project, dependencies, libraryOptions);

    for (const std::string &warning : selection.warnings) {
        warn(warning);
    }

    // Compile.
    std::vector<Unit> units = {{"", "/", &project}};
    for (const InstalledProject &dependency : dependencies) {
        units.push_back({dependency.name, std::string(kDepsRoot) + dependency.name, &dependency.project});
    }

    CompiledProgram compiled = compileUnits(units, Compiler(settings.compiler), selection);
    if (!compiled.problems.empty()) {
        std::string message = "the program can't be built:";
        for (const std::string &problem : compiled.problems) {
            message += "\n  " + problem;
        }
        throw BuildError(message);
    }

    // The bundle.
    bundle::Bundle program;
    program.name = project.manifest().name;
    program.version = project.manifest().version;
    program.entry = entry;
    program.libraries = selection.libraries;
    program.libraryHint = "add it to build.libraries in .sonata/project.luau and rebuild";
    program.modules = std::move(compiled.modules);

    for (const Unit &unit : units) {
        bundle::Package package;
        package.root = unit.root;
        package.entrypoint = unit.project->manifest().entrypoint;
        for (const Project::Dependency &dependency : unit.project->manifest().dependencies) {
            package.dependencies.push_back(dependency.name);
        }
        program.packages.push_back(std::move(package));

        if (!unit.name.empty()) {
            program.aliases.emplace_back(unit.name, unit.root);
        }

        addAssets(program, unit.root, *unit.project);
    }

    // The runtime, and the executable.
    const ChosenRuntime runtime = chooseRuntime(options, settings, selection);
    bundle::write(runtime.path, settings.output, program, settings.compress);

    std::error_code ec;
    const std::uintmax_t size = fs::file_size(settings.output, ec);

    std::cout << "built " << settings.output.generic_string() << " (" << (ec ? "?" : humanSize(size)) << ")\n";

    if (options.verbose) {
        std::cerr << "runtime   " << runtime.description << " (" << runtime.path.generic_string() << ")\n"
                  << "modules   " << program.modules.size() << '\n'
                  << "assets    " << program.assets.size() << '\n'
                  << "packages  " << (dependencies.empty() ? "none" : std::to_string(dependencies.size())) << '\n'
                  << "settings  optimize " << settings.compiler.optimizationLevel << ", debug "
                  << settings.compiler.debugLevel << ", " << (settings.compress ? "compressed" : "uncompressed")
                  << '\n';
    }

    return 0;
}

} // namespace

int build(int argc, char **argv) {
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options) {
        return 1;
    }

    try {
        return runBuild(*options);
    } catch (const std::exception &error) {
        return fail(error.what());
    }
}

const cli::Command &build_command() {
    static const cli::Command command{
        .name = "build",
        .description = "Compile a project into a standalone executable",
        .args =
            {
                cli::help_argument,
                {.name = "--output <file>", .aliases = {"-o"},
                 .description = "Where to write the program (default: build.output, or dist/<name>)"},
                {.name = "--libs <a,b,...>", .aliases = {"-l"},
                 .description = "Built-in libraries the project uses, instead of build.libraries"},
                {.name = "--all-libs", .aliases = {},
                 .description = "Include every built-in library"},
                {.name = "--optimize <0-2>", .aliases = {"-O"},
                 .description = "Luau optimization level (default 1)"},
                {.name = "--debug-level <0-2>", .aliases = {"-g"},
                 .description = "Luau debug info: 0 none, 1 line numbers (default), 2 full"},
                {.name = "--no-compress", .aliases = {},
                 .description = "Don't compress the packed program (--compress turns it back on)"},
                {.name = "--standalone", .aliases = {},
                 .description = "No shared-library dependencies at all (built from source)"},
                {.name = "--strict", .aliases = {},
                 .description = "Treat the project as --!strict for this build"},
                {.name = "--entry <file>", .aliases = {"-e"},
                 .description = "Start at <file> instead of the manifest's entrypoint"},
                {.name = "--runtime <file>", .aliases = {},
                 .description = "Use this Sonata runtime (snrt) instead of finding one"},
                {.name = "--install", .aliases = {"-i"},
                 .description = "Install missing dependencies first"},
                {.name = "--verbose", .aliases = {"-v"},
                 .description = "Show details, including the runtime's build output"},
            },
        .usage = {"sn build [path] [options]"},
        .examples = {"sn build",
                     "sn build -o bin/app",
                     "sn build --libs fs,path,term",
                     "sn build --standalone -O 2 -g 0"},

        .execute = build,

        .children = {}

    };

    return command;
}

} // namespace sonata::commands
