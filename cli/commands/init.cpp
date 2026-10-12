#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sonata/core/luau/datafile.hpp>
#include <sonata/core/project.hpp>
#include "commands/init.hpp"

namespace fs = std::filesystem;

/*  Argument Parsing:
    Parses a single positional path argument.
    Returns std::nullopt if no path was supplied or a path was already
   specified. Throws no exceptions.
*/
std::optional<fs::path> parse_path_arg(std::string_view arg,
                                       bool &pathspecified) {
    if (arg.empty())
        return std::nullopt;

    if (pathspecified)
        return std::nullopt;

    pathspecified = true;

    return fs::path(arg);
}

/*  Validation:
    Checks wether a path can be safely treated as a directory path.
    This checks every existing component of the path. If an existing
    component is a regular file, the path is invalid.

    Non-existent components are also allowed.
*/
bool validate_directory_path(const fs::path &path, std::string &error) {
    if (path.empty()) {
        error = "path is empty";
        return false;
    }

    std::error_code ec;
    fs::path current;

    // Walk through each component instead of only checking
    // the final path.
    for (const auto &component : path) {
        current /= component;

        if (!fs::exists(current, ec)) {
            if (ec) {
                error = "could not inspect path '" + current.string() +
                        "': " + ec.message();
                return false;
            }

            // This component doesn't exist. Everything after it
            // cannot be an existing file, so we're done checking.
            break;
        }

        if (!fs::is_directory(current, ec)) {
            if (ec) {
                error = "could not inspect path '" + current.string() +
                        "': " + ec.message();
                return false;
            }

            error =
                "path component is not a directory: '" + current.string() + "'";
            return false;
        }
    }

    return true;
}

enum class ExistingProjectChoice { Overwrite, Clear, Cancel };

/*  Prompting:
    Asks the user what to do when the target directory already holds a
    Sonata project. Keeps asking until a valid choice is given. If stdin
    is closed (e.g. not interactive), cancels.
*/
ExistingProjectChoice prompt_existing_project(const fs::path &root) {
    std::cout << "A Sonata project already exists at " << root << "\n"
              << "  1. Initialize anyway (overwrites main.luau and "
                 ".sonata/project.luau)\n"
              << "  2. Clear the folder and initialize\n"
              << "  3. Cancel\n";

    std::string line;
    while (true) {
        std::cout << "Choose an option [1-3]: " << std::flush;

        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            return ExistingProjectChoice::Cancel;
        }

        if (line == "1")
            return ExistingProjectChoice::Overwrite;
        if (line == "2")
            return ExistingProjectChoice::Clear;
        if (line == "3")
            return ExistingProjectChoice::Cancel;

        std::cout << "Please enter 1, 2 or 3.\n";
    }
}

/*  Clearing:
    Removes everything inside a directory, but not the directory itself.
*/
bool clear_directory(const fs::path &root, std::string &error) {
    std::error_code ec;
    std::vector<fs::path> entries;

    // Collect first, so we aren't removing entries while iterating.
    for (fs::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec))
        entries.push_back(it->path());

    if (ec) {
        error = "could not read directory '" + root.string() +
                "': " + ec.message();
        return false;
    }

    for (const auto &entry : entries) {
        fs::remove_all(entry, ec);
        if (ec) {
            error = "could not remove '" + entry.string() +
                    "': " + ec.message();
            return false;
        }
    }

    return true;
}

namespace sonata::commands {

int init(int argc, char **argv) {
    namespace fs = std::filesystem;

    fs::path specifiedpath;
    bool pathspecified = false;
    bool validate = false;

    std::vector<std::string> args(argv, argv + argc);

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];

        if (arg.empty())
            continue;

        // Options
        if (arg[0] == '-') {
            if (arg == "--validate") {
                validate = true;
            } else {
                std::cerr << "error: unknown argument " << arg << "\n";
                return 1;
            }
        }

        // Path
        else {
            auto path = parse_path_arg(arg, pathspecified);

            if (!path) {
                std::cerr << "error: unexpected argument " << arg << "\n";
                return 1;
            }

            specifiedpath = *path;
        }
    }
    fs::path root = pathspecified ? specifiedpath : fs::current_path();

    std::string patherror;

    if (!validate_directory_path(root, patherror)) {
        std::cerr << "error: " << patherror << "\n";
        return 1;
    }

    std::error_code ec;

    if (!fs::exists(root, ec)) {
        if (!fs::create_directories(root, ec)) {
            std::cerr << "error: could not create directory '" << root
                      << "': " << ec.message() << "\n";
            return 1;
        }
    }

    fs::path sonata = root / ".sonata";

    if (validate) {
        std::optional<sonata::Project> project;
        try {
            project = sonata::Project::open(".");
        } catch (const std::runtime_error &e) {
            std::cerr << "error: " << e.what() << "\n";
            std::cout << "possible fix: run 'sn init' (run 'sn init -h' to see "
                         "info)\n";
            return 1;
        }
        if (!project) {
            std::cerr << "error: project couldn't be declared/opened due to an "
                         "unknown error\n";
            return 1;
        }
        std::cout << "success\n";
        std::cout << "project root: " << project->root() << "\n";
        std::cout
            << "project manifest: "
            << luau::DataFile::parseFile(project->projectFile()).serialize()
            << "\n";
        return 0;
    }

    if (fs::exists(sonata / "project.luau", ec)) {
        switch (prompt_existing_project(root)) {
        case ExistingProjectChoice::Overwrite:
            break;
        case ExistingProjectChoice::Clear: {
            std::string clearerror;
            if (!clear_directory(root, clearerror)) {
                std::cerr << "error: " << clearerror << "\n";
                return 1;
            }
            break;
        }
        case ExistingProjectChoice::Cancel:
            std::cout << "Cancelled.\n";
            return 0;
        }
    }

    try {
        fs::create_directories(sonata / "deps");

        std::ofstream project(sonata / "project.luau");
        std::ofstream mainluau(root / "main.luau");
        luau::DataValue manifest = luau::DataValue::table();
        manifest.set("name", "myProject");
        manifest.set("version", "0.1.0");
        manifest.set("description", "A new Sonata project");
        manifest.set("entrypoint", "main.luau");
        manifest.set("authors", luau::DataValue::array({}));
        manifest.set("dependencies", luau::DataValue::array({}));
        project << "--[[\nThis file was automatically generated by Sonata.\n"
                   "It's not recommended to edit it manually, unless\n"
                   "you know what you're doing.\n]]\n\n";
        project << luau::DataFile::serialize(manifest);
        mainluau << "print('Hello World!')";
        std::cout << "Initialized Sonata project at " << root << "\n";
    } catch (const fs::filesystem_error &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}

const cli::Command &init_command() {
    static const cli::Argument validate_arg{
        .name = "--validate",
        .aliases = {},
        .description =
            "Prints if the specified directory is a valid Sonata project"};
    static const cli::Command command{
        .name = "init",
        .description = "Create a new Sonata project",
        .args = {cli::help_argument, validate_arg},
        .usage = {"sn init"},

        .examples = {"sn init"},

        .execute = init,

        .children = {}

    };

    return command;
}

} // namespace sonata::commands