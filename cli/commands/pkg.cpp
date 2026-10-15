#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sonata/core/package.hpp>
#include <sonata/core/project.hpp>
#include <sonata/core/luau/datafile.hpp>
#include "command.hpp"
#include "commands/pkg.hpp"
#include "lsp.hpp"

/*
 * sn pkg add    [path] [deps] [-r file] [-nr] [-i]
 * sn pkg remove [path] <names> [-nd] [-nm]
 * sn pkg update [path] [names]
 * sn pkg status [path] [--lua [bool]] [-nd]
 *
 * "path" is optional (default "."). It is only recognised when it is
 * unmistakably a path (see looks_like_path), so a bare name or URL is never
 * mistaken for it.
 */

namespace sonata::commands {

namespace {

namespace fs = std::filesystem;

constexpr const char *pkg_usage = "sn pkg <command> [path] [deps|names] [args]";
constexpr const char *add_usage = "sn pkg add [path] [deps] [args]";
constexpr const char *remove_usage = "sn pkg remove [path] <names> [args]";
constexpr const char *update_usage = "sn pkg update [path] [names] [args]";
constexpr const char *status_usage = "sn pkg status [path] [args]";

int add(int argc, char **argv);
int remove(int argc, char **argv);
int update(int argc, char **argv);
int status(int argc, char **argv);

// Project / manager setup
struct PkgContext {
    sonata::Project project;
    PackageManager manager;

    explicit PkgContext(sonata::Project p)
        : project(std::move(p)), manager(project) {}

    PkgContext(const PkgContext &) = delete;
    PkgContext &operator=(const PkgContext &) = delete;
};

std::unique_ptr<PkgContext> prepare(const std::string &path) {
    std::optional<sonata::Project> project;
    try {
        project = sonata::Project::open(path);
    } catch (const std::runtime_error &e) {
        std::cerr << "error: " << e.what() << "\n";
        std::cout
            << "possible fix: run 'sn init' (run 'sn init -h' to see info)\n";
        return nullptr;
    }
    if (!project) {
        std::cerr
            << "error: project couldn't be declared/opened due to an unknown "
               "error\n";
        return nullptr;
    }

    auto context = std::make_unique<PkgContext>(std::move(*project));
    context->manager.setProgressCallback(
        [](const std::string &msg) { std::cout << msg << std::endl; });

    return context;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

enum class Items { None, Optional, Required };
enum class FlagValue {
    None,        // --flag
    Required,    // --flag <value> / --flag=<value>
    OptionalBool // --flag [true|false] / --flag=<bool>; bare flag means true
};

struct FlagSpec {
    std::string name;  // long form, also the canonical name: "--nodep"
    std::string alias; // short form: "-nd" (may be empty)
    FlagValue value = FlagValue::None;
};

struct ParseSpec {
    std::string usage;
    Items items;                 // are positional deps/names accepted?
    std::string items_name;      // "deps" / "names", for error messages
    std::vector<FlagSpec> flags; // accepted flags
};

struct ParsedArgs {
    std::string path = ".";
    std::vector<std::string> items; // deps (add) or names (remove/update)
    std::set<std::string> flags;    // canonical names of the flags passed
    std::map<std::string, std::vector<std::string>> values; // flag -> values

    bool has(const std::string &flag) const { return flags.count(flag) > 0; }

    std::vector<std::string> values_of(const std::string &flag) const {
        const auto it = values.find(flag);
        return it == values.end() ? std::vector<std::string>{} : it->second;
    }

    // Value of an optional-bool flag; "fallback" if the flag wasn't passed.
    bool bool_value(const std::string &flag, bool fallback) const {
        const auto it = values.find(flag);
        if (it == values.end() || it->second.empty())
            return fallback;
        return it->second.back() == "true";
    }
};

const ParseSpec add_spec{
    .usage = add_usage,
    .items = Items::Optional, // may be omitted with --install / --read
    .items_name = "deps",
    .flags = {{.name = "--read", .alias = "-r", .value = FlagValue::Required},
              {.name = "--noreplace", .alias = "-nr"},
              {.name = "--install", .alias = "-i"}}};

const ParseSpec remove_spec{
    .usage = remove_usage,
    .items = Items::Required,
    .items_name = "names",
    .flags = {{.name = "--nodep", .alias = "-nd"},
              {.name = "--nomanifest", .alias = "-nm"}}};

const ParseSpec update_spec{.usage = update_usage,
                            .items = Items::Optional,
                            .items_name = "names",
                            .flags = {}};

const ParseSpec status_spec{
    .usage = status_usage,
    .items = Items::None,
    .items_name = "",
    .flags = {{.name = "--lua", .alias = "", .value = FlagValue::OptionalBool},
              {.name = "--nodep", .alias = "-nd"}}};

// The path is optional, so the first positional is only treated as the path
// if it is unmistakably one: it starts with "./", "/", "\" or "~", or is a
// Windows drive path (C:\... / C:/...). Anything else (e.g.
// "github.com/user/repo", or a dependency name) is a dep/name. This avoids
// guessing based on what happens to exist in the current directory.
bool looks_like_path(const std::string &s) {
    if (s.empty())
        return false;

    const char first = s[0];
    if (first == '.' || first == '/' || first == '\\' || first == '~')
        return true;

    return s.size() > 2 && s[1] == ':' && (s[2] == '/' || s[2] == '\\');
}

std::optional<bool> parse_bool(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (text == "true" || text == "yes" || text == "on" || text == "1")
        return true;
    if (text == "false" || text == "no" || text == "off" || text == "0")
        return false;
    return std::nullopt;
}

const FlagSpec *find_flag(const ParseSpec &spec, const std::string &token) {
    for (const auto &flag : spec.flags) {
        if (token == flag.name || (!flag.alias.empty() && token == flag.alias))
            return &flag;
    }
    return nullptr;
}

// Parses "argv" (the arguments after the subcommand) into "out".
//
// Returns std::nullopt if parsing succeeded and the command should continue.
// Otherwise returns the exit code the command should return immediately
// (1 after printing an error). Help flags are handled by the CLI before a
// command runs, so they never reach this function.
std::optional<int> parse_args(int argc, char **argv, const ParseSpec &spec,
                              ParsedArgs &out) {
    std::vector<std::string> positionals;
    bool only_positionals = false; // set after a bare "--"

    for (int i = 0; i < argc; ++i) {
        const std::string token = argv[i];

        if (!only_positionals && token == "--") {
            only_positionals = true;
            continue;
        }

        // Not a flag: a lone "-" or anything not starting with '-'.
        if (only_positionals || token.size() < 2 || token[0] != '-') {
            positionals.push_back(token);
            continue;
        }

        // "--flag=value" is accepted as well as "--flag value".
        std::string key = token;
        std::optional<std::string> inline_value;
        if (const auto eq = token.find('='); eq != std::string::npos) {
            key = token.substr(0, eq);
            inline_value = token.substr(eq + 1);
        }

        const FlagSpec *flag = find_flag(spec, key);
        if (!flag) {
            std::cerr << "error: unknown argument '" << token << "'\n"
                      << "usage: " << spec.usage << "\n";
            return 1;
        }
        out.flags.insert(flag->name);

        switch (flag->value) {
        case FlagValue::None:
            if (inline_value) {
                std::cerr << "error: '" << flag->name
                          << "' doesn't take a value\n";
                return 1;
            }
            break;

        case FlagValue::Required:
            if (inline_value) {
                out.values[flag->name].push_back(*inline_value);
            } else if (i + 1 < argc) {
                out.values[flag->name].push_back(argv[++i]);
            } else {
                std::cerr << "error: '" << flag->name << "' needs a value\n"
                          << "usage: " << spec.usage << "\n";
                return 1;
            }
            break;

        case FlagValue::OptionalBool: {
            std::optional<bool> value;
            if (inline_value) {
                value = parse_bool(*inline_value);
                if (!value) {
                    std::cerr << "error: '" << flag->name
                              << "' expects true or false, got '"
                              << *inline_value << "'\n";
                    return 1;
                }
            } else if (i + 1 < argc) {
                // Only swallow the next token if it is a boolean word.
                value = parse_bool(argv[i + 1]);
                if (value)
                    ++i;
            }
            out.values[flag->name].push_back(value.value_or(true) ? "true"
                                                                  : "false");
            break;
        }
        }
    }

    if (spec.items == Items::None) {
        // Only an (optional) path; with no deps/names there is nothing to
        // confuse it with, so no need for looks_like_path().
        if (positionals.size() > 1) {
            std::cerr << "error: unexpected argument '" << positionals[1]
                      << "'\nusage: " << spec.usage << "\n";
            return 1;
        }
        if (!positionals.empty())
            out.path = positionals[0];
        return std::nullopt;
    }

    // [path] [items...]
    auto first_item = positionals.begin();
    if (first_item != positionals.end() && looks_like_path(*first_item)) {
        out.path = *first_item;
        ++first_item;
    }
    out.items.assign(first_item, positionals.end());

    if (spec.items == Items::Required && out.items.empty()) {
        std::cerr << "error: missing <" << spec.items_name << ">\n"
                  << "usage: " << spec.usage << "\n";
        return 1;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Names and URLs
// ---------------------------------------------------------------------------

bool is_name_char(unsigned char c) { return std::isalnum(c) || c == '_'; }

// Same rule as Project::Dependency: letters, digits, '_'; no leading digit.
bool is_valid_name(const std::string &name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return is_name_char(static_cast<unsigned char>(c));
    });
}

std::string extractRepoName(const std::string &url) {
    if (url.empty()) {
        return "";
    }

    std::string repoName = url;

    // 1. Strip any trailing slashes
    while (!repoName.empty() &&
           (repoName.back() == '/' || repoName.back() == '\\')) {
        repoName.pop_back();
    }

    // 2. Strip the ".git" extension if it exists
    const std::string gitSuffix = ".git";
    if (repoName.length() >= gitSuffix.length() &&
        repoName.compare(repoName.length() - gitSuffix.length(),
                         gitSuffix.length(), gitSuffix) == 0) {
        repoName.erase(repoName.length() - gitSuffix.length());
    }

    // 3. Find the last separator ('/', '\', or ':')
    // The ':' is crucial for parsing SCP-like SSH URLs (e.g.,
    // git@github.com:user/repo.git)
    size_t lastSeparator = repoName.find_last_of("/:\\");
    if (lastSeparator != std::string::npos) {
        repoName = repoName.substr(lastSeparator + 1);
    }

    return repoName;
}

// Repository names are often not valid dependency names ("my-repo",
// "2d.lib"): turn every other character into '_' and guard a leading digit.
std::string sanitize_name(std::string name) {
    for (char &c : name) {
        if (!is_name_char(static_cast<unsigned char>(c)))
            c = '_';
    }
    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0])))
        name.insert(name.begin(), '_');
    return name;
}

// "github.com/user/repo" -> "https://github.com/user/repo". Git itself would
// treat the former as a local path. Anything with a scheme, an scp-like
// "git@host:path", a drive letter or a leading ./ ../ / ~ is left untouched.
std::string add_default_scheme(const std::string &url) {
    if (url.find("://") != std::string::npos)
        return url;

    const auto slash = url.find('/');
    if (slash == std::string::npos || slash == 0)
        return url;

    const std::string host = url.substr(0, slash);
    const bool hostlike = host.find('.') != std::string::npos &&
                          host.find_first_of(":@\\") == std::string::npos &&
                          host[0] != '.' && host[0] != '~';
    return hostlike ? "https://" + url : url;
}

// "<url>[#<ref>]" -> Dependency. The name is derived from the repository.
Project::Dependency dependency_from_spec(const std::string &spec) {
    Project::Dependency dep;
    std::string url = spec;
    if (const auto hash = url.rfind('#'); hash != std::string::npos) {
        dep.ref = url.substr(hash + 1);
        url.erase(hash);
    }
    dep.name = sanitize_name(extractRepoName(url));
    dep.url = add_default_scheme(url);
    return dep;
}

std::string describe_source(const Project::Dependency &dep) {
    return dep.ref.empty() ? dep.url : dep.url + "#" + dep.ref;
}

template <typename Range>
std::string join(const Range &range, const std::string &separator = ", ") {
    std::string out;
    for (const auto &item : range) {
        if (!out.empty())
            out += separator;
        out += item;
    }
    return out;
}

bool contains(const std::vector<std::string> &list, const std::string &item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// Names without "*" or duplicates, in the order they were given.
std::vector<std::string> unique_names(const std::vector<std::string> &items) {
    std::vector<std::string> out;
    for (const auto &item : items) {
        if (item != "*" && !contains(out, item))
            out.push_back(item);
    }
    return out;
}

std::string short_commit(const std::string &commit) {
    return commit.size() > 10 ? commit.substr(0, 10) : commit;
}

// ---------------------------------------------------------------------------
// --read <file>
// ---------------------------------------------------------------------------

struct Requested {
    Project::Dependency dep;
    std::string origin; // for messages: the argument, or "<file> (entry N)"
};

const std::string *string_field(const luau::DataValue &table,
                                const std::string &key) {
    const luau::DataValue *value = table.find(key);
    return (value && value->isString()) ? &value->asString() : nullptr;
}

// Reads a Luau table of dependencies. Every member is either
//
//     "https://github.com/acme/json.git"                  (name is derived)
//     "https://github.com/acme/json.git#v1.2.0"           (with a ref)
//     { name = "json", url = "https://...", ref = "v1" }  (name/ref optional)
//
// A member's own key ("json = ...") is used as the name if none is given.
//
// Appends the usable entries to "out" and returns how many were unusable
// (each already reported on stderr). Throws if the file can't be read or
// isn't a literal table.
int read_dependency_file(const std::string &file, std::vector<Requested> &out) {
    const luau::DataValue table = luau::DataFile::parseFile(file);

    int bad = 0;
    int index = 0;
    for (const auto &entry : table.asTable()) {
        ++index;
        const std::string origin =
            file + " (entry " + std::to_string(index) + ")";

        std::string key;
        if (entry.key &&
            entry.key->kind == luau::DataValue::Key::Kind::String) {
            key = entry.key->string;
        }

        if (entry.value.isString()) {
            Project::Dependency dep =
                dependency_from_spec(entry.value.asString());
            if (!key.empty())
                dep.name = key;
            out.push_back({std::move(dep), origin});
        } else if (entry.value.isTable()) {
            const std::string *url = string_field(entry.value, "url");
            if (!url) {
                std::cerr << "error: " << origin
                          << ": expected a string 'url'\n";
                ++bad;
                continue;
            }
            Project::Dependency dep = dependency_from_spec(*url);
            if (const std::string *name = string_field(entry.value, "name"))
                dep.name = *name;
            else if (!key.empty())
                dep.name = key;
            if (const std::string *ref = string_field(entry.value, "ref"))
                dep.ref = *ref;
            out.push_back({std::move(dep), origin});
        } else {
            std::cerr << "error: " << origin
                      << ": expected a URL string or a table with 'name' and "
                         "'url'\n";
            ++bad;
        }
    }
    return bad;
}

// ---------------------------------------------------------------------------
// Installed packages and their relationships
// ---------------------------------------------------------------------------

// package name -> names of the packages it depends on (its own manifest)
using DepGraph = std::map<std::string, std::set<std::string>>;

std::string package_name(const InstalledPackage &p) {
    return p.dependency.name.empty() ? p.path.filename().string()
                                     : p.dependency.name;
}

std::map<std::string, InstalledPackage>
installed_map(const PackageManager &manager) {
    std::map<std::string, InstalledPackage> out;
    for (const auto &p : manager.installed())
        out.emplace(package_name(p), p);
    return out;
}

std::map<std::string, std::string>
commit_snapshot(const PackageManager &manager) {
    std::map<std::string, std::string> out;
    for (const auto &p : manager.installed())
        out[package_name(p)] = p.commit;
    return out;
}

DepGraph build_graph(const std::map<std::string, InstalledPackage> &installed) {
    DepGraph graph;
    for (const auto &[name, pkg] : installed) {
        auto &edges = graph[name];
        try {
            const Project dependency = Project::open(pkg.path);
            for (const auto &d : dependency.manifest().dependencies)
                edges.insert(d.name);
        } catch (const std::exception &) {
            // Unreadable manifest: treated as having no dependencies.
        }
    }
    return graph;
}

// Packages (other than those in "ignore") that depend on "name".
std::set<std::string> users_of(const DepGraph &graph, const std::string &name,
                               const std::set<std::string> &ignore = {}) {
    std::set<std::string> users;
    for (const auto &[pkg, deps] : graph) {
        if (!ignore.count(pkg) && deps.count(name))
            users.insert(pkg);
    }
    return users;
}

// Everything reachable from "start" (including "start" itself), never
// stepping onto a package in "blocked".
std::set<std::string> reachable(const DepGraph &graph,
                                const std::vector<std::string> &start,
                                const std::set<std::string> &blocked = {}) {
    std::set<std::string> seen;
    std::vector<std::string> stack;
    for (const auto &s : start) {
        if (!blocked.count(s))
            stack.push_back(s);
    }
    while (!stack.empty()) {
        const std::string current = std::move(stack.back());
        stack.pop_back();
        if (!seen.insert(current).second)
            continue;
        const auto it = graph.find(current);
        if (it == graph.end())
            continue;
        for (const auto &next : it->second) {
            if (!blocked.count(next) && !seen.count(next))
                stack.push_back(next);
        }
    }
    return seen;
}

bool delete_package_folder(const InstalledPackage &pkg) {
    std::error_code ec;
    fs::remove_all(pkg.path, ec);
    if (ec) {
        std::cerr << "error: couldn't delete " << pkg.path.string() << ": "
                  << ec.message() << "\n";
        return false;
    }
    return true;
}

void explain_unknown_name(const PackageManager &manager,
                          const std::string &name) {
    if (!is_valid_name(name)) {
        std::cerr << "error: '" << name
                  << "' is not a valid dependency name (dependencies are "
                     "removed/updated by name, not by URL)\n";
        return;
    }
    if (manager.isInstalled(name)) {
        const auto users = users_of(build_graph(installed_map(manager)), name);
        std::cerr << "error: '" << name << "' is not listed in the manifest";
        if (!users.empty())
            std::cerr << "; it is only installed as a dependency of "
                      << join(users);
        std::cerr << "\n";
        return;
    }
    std::cerr << "error: '" << name
              << "' is not a dependency of this project\n";
}

// ---------------------------------------------------------------------------
// Command descriptions
// ---------------------------------------------------------------------------

const cli::Command &add_command() {
    static const cli::Argument read_arg{
        .name = "--read, -r <file>",
        .description =
            "Reads a Luau table from <file> and adds every dependency in it. "
            "A member is either a URL string or a table with 'name' and "
            "'url' (and optionally 'ref'). Can be given more than once."};

    static const cli::Argument noreplace_arg{
        .name = "--noreplace, -nr",
        .description =
            "Leaves dependencies that are already installed (including "
            "dependencies of dependencies) untouched."};

    static const cli::Argument install_arg{
        .name = "--install, -i",
        .description =
            "Installs every dependency listed in the project manifest. Any "
            "<deps> given as well are added alongside them."};

    static const cli::Command command{
        .name = "add [path] [deps]",
        .description =
            "Adds the packages (and their dependencies) from the given "
            "repository URLs (url or url#ref) to the project at [path] "
            "(default '.'). <deps> can be left out with --install or --read.",
        .args = {read_arg, noreplace_arg, install_arg},
        .usage = {add_usage},

        .examples = {"sn pkg add github.com/user/repo",
                     "sn pkg add https://github.com/user/repo#v1.2.0",
                     "sn pkg add ./my_project github.com/user/repo -nr",
                     "sn pkg add . github.com/user/repo github.com/user/other",
                     "sn pkg add --read deps.luau", "sn pkg add -i"},

        .execute = add};

    return command;
}

const cli::Command &remove_command() {
    static const cli::Argument nodep_arg{
        .name = "--nodep, -nd",
        .description =
            "Only removes the named dependencies, not the dependencies "
            "they brought in."};

    static const cli::Argument nomanifest_arg{
        .name = "--nomanifest, -nm",
        .description = "Leaves the project manifest alone and only deletes "
                       "the dependency folders."};

    static const cli::Command command{
        .name = "remove [path] <names>",
        .description =
            "Removes the named dependencies from the project at [path] "
            "(default '.'), along with dependencies that nothing else uses. "
            "'*' removes every package (quote it so the shell doesn't "
            "expand it).",
        .args = {nodep_arg, nomanifest_arg},
        .usage = {remove_usage},

        .examples = {"sn pkg remove json", "sn pkg remove . json util",
                     "sn pkg remove ./my_project json --nodep",
                     "sn pkg remove json --nomanifest", "sn pkg remove '*'"},

        .execute = remove};

    return command;
}

const cli::Command &update_command() {
    static const cli::Command command{
        .name = "update [path] [names]",
        .description =
            "Updates the packages of the project at [path] (default '.'). "
            "If names are given, only those are updated (default '*').",
        .usage = {update_usage},

        .examples = {"sn pkg update", "sn pkg update ./my_project",
                     "sn pkg update . json util"},

        .execute = update};

    return command;
}

const cli::Command &status_command() {
    static const cli::Argument lua_arg{
        .name = "--lua [true|false]",
        .description =
            "Prints the data as a Luau table. The bool (default true) says "
            "whether to start the output with 'return '."};

    static const cli::Argument nodep_arg{
        .name = "--nodep, -nd",
        .description = "Only lists the project's own dependencies, not the "
                       "dependencies of dependencies."};

    static const cli::Command command{
        .name = "status [path]",
        .description =
            "Lists every dependency of the project at [path] (default '.') "
            "with its version, source, lock info, what it depends on and "
            "what uses it.",
        .args = {lua_arg, nodep_arg},
        .usage = {status_usage},

        .examples = {"sn pkg status", "sn pkg status ./my_project --nodep",
                     "sn pkg status --lua", "sn pkg status --lua false"},

        .execute = status};

    return command;
}

// ---------------------------------------------------------------------------
// add
// ---------------------------------------------------------------------------

int add(int argc, char **argv) {
    ParsedArgs args;
    if (const auto code = parse_args(argc, argv, add_spec, args))
        return *code;

    const bool install_all = args.has("--install");
    const bool no_replace = args.has("--noreplace");
    const std::vector<std::string> read_files = args.values_of("--read");

    if (args.items.empty() && read_files.empty() && !install_all) {
        std::cerr << "error: missing <deps> (give a repository URL, or use "
                     "--read / --install)\n"
                  << "usage: " << add_usage << "\n";
        return 1;
    }

    auto context = prepare(args.path);
    if (!context)
        return 1;
    Project &project = context->project;
    PackageManager &manager = context->manager;

    // Collect everything first, so a broken --read file is reported before
    // anything has been installed.
    std::vector<Requested> requests;
    int failures = 0;
    for (const auto &spec : args.items)
        requests.push_back({dependency_from_spec(spec), spec});
    for (const auto &file : read_files) {
        try {
            failures += read_dependency_file(file, requests);
        } catch (const std::exception &e) {
            std::cerr << "error: couldn't read '" << file << "': " << e.what()
                      << "\n";
            return 1;
        }
    }
    const int total = static_cast<int>(requests.size()) + failures;

    // --install: everything in the manifest first.
    if (install_all) {
        std::cout << "installing the dependencies in the manifest\n";
        try {
            const auto packages = manager.install();
            const auto fetched = std::count_if(
                packages.begin(), packages.end(),
                [](const InstalledPackage &p) { return p.fresh; });
            std::cout << packages.size() << " package(s) in place, " << fetched
                      << " fetched\n";
        } catch (const std::exception &e) {
            std::cerr << "installation error: " << e.what() << "\n";
            ++failures;
        }
    }

    // For --noreplace: what was there before we started.
    const auto before = no_replace ? commit_snapshot(manager)
                                   : std::map<std::string, std::string>{};

    int added = 0;
    int skipped = 0;
    for (const auto &request : requests) {
        const Project::Dependency &dep = request.dep;

        if (const auto error = dep.validationError()) {
            std::cerr << "validation error: " << *error << " in "
                      << request.origin << "\n";
            ++failures;
            continue;
        }

        if (no_replace) {
            const Project::Dependency *existing =
                project.findDependency(dep.name);
            if (manager.isInstalled(dep.name)) {
                std::cout << "skipped " << dep.name << " (already installed)\n";
                ++skipped;
                continue;
            }
            if (existing && !existing->sameSource(dep)) {
                std::cout << "skipped " << dep.name
                          << " (the manifest already lists it from "
                          << describe_source(*existing) << ")\n";
                ++skipped;
                continue;
            }
        }

        try {
            const InstalledPackage installed = manager.add(dep);
            std::cout << "added " << package_name(installed);
            if (!installed.commit.empty())
                std::cout << " (" << short_commit(installed.commit) << ")";
            std::cout << "\n";
            ++added;
        } catch (const std::exception &e) {
            std::cerr << "installation error: " << e.what() << " in "
                      << request.origin << "\n";
            ++failures;
        }
    }

    // PackageManager has no "don't touch what's there" switch of its own, so
    // check that nothing that existed before has moved.
    if (no_replace) {
        for (const auto &[name, commit] : before) {
            const auto now = commit_snapshot(manager);
            const auto it = now.find(name);
            if (it != now.end() && !commit.empty() && !it->second.empty() &&
                it->second != commit) {
                std::cerr << "warning: '" << name
                          << "' changed during the install although "
                             "--noreplace was given (was "
                          << short_commit(commit) << ", now "
                          << short_commit(it->second) << ")\n";
            }
        }
    }

    if (total > 0) {
        std::cout << added << " out of " << total
                  << " dependencies successfully installed";
        if (skipped > 0)
            std::cout << " (" << skipped << " skipped)";
        std::cout << "\n";
    }
    lsp::refresh(project.root(), std::cerr);
    return failures > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// remove
// ---------------------------------------------------------------------------

// Default: take the names out of the manifest and let the manager delete them
// together with everything that only they needed.
int remove_cascading(Project &project, PackageManager &manager,
                     const std::vector<std::string> &names, bool everything) {
    int failures = 0;

    std::vector<std::string> targets = names;
    if (everything) {
        targets.clear();
        for (const auto &d : project.manifest().dependencies)
            targets.push_back(d.name);
        if (targets.empty() && manager.installed().empty()) {
            std::cout << "nothing to remove\n";
            return 0;
        }
    }

    for (const auto &name : targets) {
        if (!project.findDependency(name)) {
            explain_unknown_name(manager, name);
            ++failures;
            continue;
        }
        try {
            const std::vector<std::string> deleted = manager.remove(name);
            std::cout << "removed " << name << " from the manifest\n";
            for (const auto &folder : deleted)
                std::cout << "  deleted " << folder << "\n";

            if (manager.isInstalled(name)) {
                const auto users =
                    users_of(build_graph(installed_map(manager)), name);
                std::cout << "  kept " << name << ": ";
                if (users.empty())
                    std::cout << "it is still needed\n";
                else
                    std::cout << "still required by " << join(users) << "\n";
            }
        } catch (const std::exception &e) {
            std::cerr << "error: couldn't remove '" << name << "': " << e.what()
                      << "\n";
            ++failures;
        }
    }

    if (everything) {
        try {
            for (const auto &folder : manager.prune())
                std::cout << "  deleted " << folder << "\n";
        } catch (const std::exception &e) {
            std::cerr << "error: " << e.what() << "\n";
            ++failures;
        }
    }
    return failures;
}

// --nodep: take the names out of the manifest and delete only their own
// folders (and only if no other package needs them). Their dependencies stay.
int remove_shallow(Project &project, PackageManager &manager,
                   const std::vector<std::string> &names) {
    const auto installed = installed_map(manager);
    const DepGraph graph = build_graph(installed);
    const std::set<std::string> targets(names.begin(), names.end());

    int failures = 0;
    for (const auto &name : names) {
        if (!project.findDependency(name)) {
            explain_unknown_name(manager, name);
            ++failures;
            continue;
        }
        try {
            project.removeDependency(name);
        } catch (const std::exception &e) {
            std::cerr << "error: couldn't update the manifest: " << e.what()
                      << "\n";
            ++failures;
            continue;
        }
        std::cout << "removed " << name << " from the manifest\n";

        const auto it = installed.find(name);
        if (it == installed.end())
            continue; // nothing on disk

        const auto users = users_of(graph, name, targets);
        if (!users.empty()) {
            std::cout << "  kept " << name << ": still required by "
                      << join(users) << "\n";
        } else if (delete_package_folder(it->second)) {
            std::cout << "  deleted " << name << "\n";
        } else {
            ++failures;
        }

        std::vector<std::string> kept;
        if (const auto edges = graph.find(name); edges != graph.end()) {
            for (const auto &dep : edges->second) {
                if (installed.count(dep) && !targets.count(dep))
                    kept.push_back(dep);
            }
        }
        if (!kept.empty())
            std::cout << "  kept dependencies: " << join(kept) << "\n";
    }
    return failures;
}

// --nomanifest: delete folders, leave the manifest (and lock file) alone, so
// "sn pkg add -i" brings them back exactly as locked.
int remove_folders(Project &project, PackageManager &manager,
                   const std::vector<std::string> &names, bool everything,
                   bool no_dep) {
    const auto installed = installed_map(manager);
    const DepGraph graph = build_graph(installed);

    int failures = 0;
    std::set<std::string> doomed;

    if (everything) {
        for (const auto &[name, pkg] : installed)
            doomed.insert(name);
    } else {
        std::set<std::string> targets;
        for (const auto &name : names) {
            if (installed.count(name)) {
                targets.insert(name);
            } else if (project.findDependency(name)) {
                std::cout << name << " is not installed, nothing to delete\n";
            } else {
                explain_unknown_name(manager, name);
                ++failures;
            }
        }

        doomed = targets;
        if (!no_dep) {
            // Also delete what only the targets needed: everything below
            // them that the rest of the project can't reach without them.
            std::vector<std::string> roots;
            for (const auto &d : project.manifest().dependencies) {
                if (!targets.count(d.name))
                    roots.push_back(d.name);
            }
            const auto survivors = reachable(graph, roots, targets);
            const std::vector<std::string> start(targets.begin(),
                                                 targets.end());
            for (const auto &name : reachable(graph, start)) {
                if (installed.count(name) && !survivors.count(name))
                    doomed.insert(name);
            }
        }
    }

    for (const auto &name : doomed) {
        if (delete_package_folder(installed.at(name)))
            std::cout << "deleted " << name << "\n";
        else
            ++failures;
    }

    if (doomed.empty())
        std::cout << "nothing to delete\n";
    else
        std::cout << "the manifest was not changed; 'sn pkg add -i' "
                     "reinstalls what was deleted\n";
    return failures;
}

int remove(int argc, char **argv) {
    ParsedArgs args;
    if (const auto code = parse_args(argc, argv, remove_spec, args))
        return *code;

    auto context = prepare(args.path);
    if (!context)
        return 1;
    Project &project = context->project;
    PackageManager &manager = context->manager;

    const bool everything = contains(args.items, "*");
    const bool no_dep = args.has("--nodep");
    const bool no_manifest = args.has("--nomanifest");
    const std::vector<std::string> names = unique_names(args.items);

    if (everything && no_dep)
        std::cout << "note: --nodep has no effect together with '*'\n";

    int failures = 0;
    if (no_manifest)
        failures = remove_folders(project, manager, names, everything, no_dep);
    else if (no_dep && !everything)
        failures = remove_shallow(project, manager, names);
    else
        failures = remove_cascading(project, manager, names, everything);

    lsp::refresh(project.root(), std::cerr);
    return failures > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------

int update(int argc, char **argv) {
    ParsedArgs args;
    if (const auto code = parse_args(argc, argv, update_spec, args))
        return *code;

    auto context = prepare(args.path);
    if (!context)
        return 1;
    PackageManager &manager = context->manager;

    const bool everything = args.items.empty() || contains(args.items, "*");

    auto known = commit_snapshot(manager); // name -> commit before the update
    int updated = 0;
    int unchanged = 0;
    int failures = 0;

    // Only packages that were actually fetched by the call are reported.
    auto report = [&](const std::vector<InstalledPackage> &result) {
        for (const auto &pkg : result) {
            if (!pkg.fresh)
                continue;
            const std::string name = package_name(pkg);
            const auto old = known.find(name);

            if (old == known.end()) {
                std::cout << "  installed " << name << " ("
                          << short_commit(pkg.commit) << ")\n";
                ++updated;
            } else if (old->second == pkg.commit) {
                std::cout << "  " << name << " is already at "
                          << short_commit(pkg.commit) << "\n";
                ++unchanged;
            } else {
                std::cout << "  updated " << name << ": "
                          << (old->second.empty() ? "(unknown)"
                                                  : short_commit(old->second))
                          << " -> " << short_commit(pkg.commit) << "\n";
                ++updated;
            }
            known[name] = pkg.commit;
        }
    };

    if (everything) {
        try {
            report(manager.update());
        } catch (const std::exception &e) {
            std::cerr << "update error: " << e.what() << "\n";
            ++failures;
        }
    } else {
        for (const auto &name : unique_names(args.items)) {
            if (!is_valid_name(name)) {
                explain_unknown_name(manager, name);
                ++failures;
                continue;
            }
            try {
                report(manager.update(name));
            } catch (const std::exception &e) {
                std::cerr << "update error: " << e.what() << " (" << name
                          << ")\n";
                ++failures;
            }
        }
    }

    std::cout << updated << " updated, " << unchanged << " unchanged";
    if (failures > 0)
        std::cout << ", " << failures << " failed";
    std::cout << "\n";
    lsp::refresh(context->project.root(), std::cerr);
    return failures > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// status
// ---------------------------------------------------------------------------

struct PackageInfo {
    std::string name;
    bool direct = false;    // listed in the project's own manifest
    bool installed = false; // present in .sonata/deps
    std::string state;      // ok / missing / mismatch / extraneous / unlocked

    std::string version; // from the package's own manifest

    // Where it should come from: the project manifest for direct
    // dependencies, otherwise the first package that asks for it.
    std::string url;
    std::string ref;

    // What the lock file recorded for it.
    std::string lock_url;
    std::string lock_ref;
    std::string commit;
    bool lock_differs = false; // lock source isn't the declared source

    fs::path path;
    std::set<std::string> dependencies; // what it needs
    std::set<std::string> used_by;      // packages that need it
    std::string note;
};

std::vector<PackageInfo> collect_packages(const Project &project,
                                          const PackageManager &manager) {
    std::map<std::string, PackageInfo> nodes;
    auto node = [&nodes](const std::string &name) -> PackageInfo & {
        PackageInfo &info = nodes[name];
        info.name = name;
        return info;
    };

    std::vector<std::string> order; // the project's own, in manifest order
    for (const auto &dep : project.manifest().dependencies) {
        PackageInfo &info = node(dep.name);
        info.direct = true;
        info.url = dep.url;
        info.ref = dep.ref;
        order.push_back(dep.name);
    }

    for (const auto &[name, pkg] : installed_map(manager)) {
        PackageInfo &info = node(name);
        info.installed = true;
        info.path = pkg.path;
        info.commit = pkg.commit;
        info.lock_url = pkg.dependency.url;
        info.lock_ref = pkg.dependency.ref;

        try {
            const Project dependency = Project::open(pkg.path);
            info.version = dependency.manifest().version;
            for (const auto &d : dependency.manifest().dependencies) {
                info.dependencies.insert(d.name);
                PackageInfo &child = node(d.name);
                child.used_by.insert(name);
                if (!child.direct && child.url.empty()) {
                    child.url = d.url;
                    child.ref = d.ref;
                }
            }
        } catch (const std::exception &e) {
            info.note = std::string("couldn't read its manifest: ") + e.what();
        }
    }

    for (auto &[name, info] : nodes) {
        if (info.url.empty()) { // nobody declares it: all we know is the lock
            info.url = info.lock_url;
            info.ref = info.lock_ref;
        }
        if (info.installed && !info.lock_url.empty() && !info.url.empty()) {
            const Project::Dependency declared{name, info.url, info.ref};
            const Project::Dependency locked{name, info.lock_url,
                                             info.lock_ref};
            info.lock_differs = !declared.sameSource(locked);
        }

        if (!info.installed)
            info.state = "missing";
        else if (info.lock_differs)
            info.state = "mismatch";
        else if (!info.direct && info.used_by.empty())
            info.state = "extraneous";
        else if (info.commit.empty())
            info.state = "unlocked";
        else
            info.state = "ok";
    }

    std::vector<PackageInfo> result;
    std::set<std::string> emitted;
    for (const auto &name : order) {
        if (emitted.insert(name).second)
            result.push_back(nodes[name]);
    }
    for (auto &[name, info] : nodes) {
        if (emitted.insert(name).second)
            result.push_back(std::move(info));
    }
    return result;
}

std::string display_path(const fs::path &path, const fs::path &base) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, base, ec);
    return (ec || relative.empty()) ? path.generic_string()
                                    : relative.generic_string();
}

void print_field(const std::string &label, const std::string &value) {
    std::cout << "    " << std::left << std::setw(12) << (label + ":") << value
              << "\n";
}

std::string describe_lock(const PackageInfo &p) {
    if (!p.installed)
        return "(not installed)";
    if (p.commit.empty() && p.lock_url.empty())
        return "(not in the lock file)";

    std::string out = p.commit.empty() ? "(commit unknown)" : p.commit;
    if (p.lock_differs) {
        out += "  locked from " + p.lock_url;
        if (!p.lock_ref.empty())
            out += "#" + p.lock_ref;
    }
    return out;
}

void print_status_text(const Project &project, const PackageManager &manager,
                       const std::vector<PackageInfo> &packages,
                       bool show_deps) {
    const fs::path &root = project.root();
    std::error_code ec;
    const bool has_lock = fs::exists(manager.lockFile(), ec);

    const Project::Manifest &manifest = project.manifest();
    std::cout << "project:   " << manifest.name;
    if (!manifest.version.empty())
        std::cout << " " << manifest.version;
    std::cout << "\nroot:      " << root.generic_string()
              << "\nlock file: " << display_path(manager.lockFile(), root)
              << (has_lock ? "" : " (not found)") << "\n";

    const auto direct =
        std::count_if(packages.begin(), packages.end(),
                      [](const PackageInfo &p) { return p.direct; });
    std::cout << "packages:  " << direct << " direct";
    if (show_deps)
        std::cout << ", " << (packages.size() - direct) << " transitive";
    std::cout << "\n";

    for (const auto &p : packages) {
        std::cout << "\n" << p.name;
        if (!p.version.empty())
            std::cout << " " << p.version;
        std::cout << "  [" << p.state << "]  "
                  << (p.direct ? "direct" : "transitive") << "\n";

        print_field("url", p.url.empty() ? "(unknown)" : p.url);
        print_field("ref", p.ref.empty() ? "(default branch)" : p.ref);
        print_field("lock", describe_lock(p));
        if (p.installed)
            print_field("path", display_path(p.path, root));
        if (show_deps)
            print_field("depends on", p.dependencies.empty()
                                          ? "(nothing)"
                                          : join(p.dependencies));

        std::vector<std::string> users;
        if (p.direct)
            users.push_back("(project)");
        users.insert(users.end(), p.used_by.begin(), p.used_by.end());
        print_field("used by", users.empty() ? "(nothing)" : join(users));

        if (!p.note.empty())
            print_field("note", p.note);
    }
}

luau::DataValue string_list(const std::set<std::string> &names) {
    luau::DataValue list = luau::DataValue::table();
    for (const auto &name : names)
        list.push(name);
    return list;
}

luau::DataValue package_data(const PackageInfo &p, const fs::path &root,
                             bool show_deps) {
    luau::DataValue out = luau::DataValue::table();
    out.set("name", p.name);
    out.set("direct", p.direct);
    out.set("installed", p.installed);
    out.set("state", p.state);
    if (!p.version.empty())
        out.set("version", p.version);
    if (!p.url.empty())
        out.set("url", p.url);
    if (!p.ref.empty())
        out.set("ref", p.ref);
    if (p.installed) {
        out.set("path", display_path(p.path, root));

        luau::DataValue lock = luau::DataValue::table();
        if (!p.lock_url.empty())
            lock.set("url", p.lock_url);
        if (!p.lock_ref.empty())
            lock.set("ref", p.lock_ref);
        if (!p.commit.empty())
            lock.set("commit", p.commit);
        out.set("lock", std::move(lock));
    }
    if (show_deps)
        out.set("dependencies", string_list(p.dependencies));
    out.set("usedBy", string_list(p.used_by));
    if (!p.note.empty())
        out.set("note", p.note);
    return out;
}

luau::DataValue status_data(const Project &project,
                            const PackageManager &manager,
                            const std::vector<PackageInfo> &packages,
                            bool show_deps) {
    const fs::path &root = project.root();
    std::error_code ec;

    luau::DataValue info = luau::DataValue::table();
    info.set("name", project.manifest().name);
    if (!project.manifest().version.empty())
        info.set("version", project.manifest().version);
    info.set("root", root.generic_string());
    info.set("lockFile", display_path(manager.lockFile(), root));
    info.set("locked", fs::exists(manager.lockFile(), ec));

    luau::DataValue list = luau::DataValue::table();
    for (const auto &p : packages)
        list.push(package_data(p, root, show_deps));

    luau::DataValue document = luau::DataValue::table();
    document.set("project", std::move(info));
    document.set("packages", std::move(list));
    return document;
}

int status(int argc, char **argv) {
    ParsedArgs args;
    if (const auto code = parse_args(argc, argv, status_spec, args))
        return *code;

    auto context = prepare(args.path);
    if (!context)
        return 1;

    const bool show_deps = !args.has("--nodep");

    std::vector<PackageInfo> packages =
        collect_packages(context->project, context->manager);
    if (!show_deps) {
        packages.erase(
            std::remove_if(packages.begin(), packages.end(),
                           [](const PackageInfo &p) { return !p.direct; }),
            packages.end());
    }

    if (args.has("--lua")) {
        const bool with_return = args.bool_value("--lua", true);
        const luau::DataValue document = status_data(
            context->project, context->manager, packages, show_deps);
        std::cout << (with_return ? luau::DataFile::serialize(document)
                                  : document.serialize() + "\n");
    } else {
        print_status_text(context->project, context->manager, packages,
                          show_deps);
    }
    return 0;
}

} // namespace

int pkg(int argc, char **argv) {
    if (argc == 0) {
        std::cerr << "usage: " << pkg_usage << "\n";
        return 1;
    }

    const std::string command = argv[0];

    try {
        if (command == "add")
            return add(argc - 1, argv + 1);

        if (command == "remove")
            return remove(argc - 1, argv + 1);

        if (command == "update")
            return update(argc - 1, argv + 1);

        if (command == "status")
            return status(argc - 1, argv + 1);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "error: unknown pkg command '" << command << "'\n";
    return 1;
}

const cli::Command &pkg_command() {
    static const cli::Command command{
        .name = "pkg",
        .description = "Manage Sonata packages",
        .usage = {pkg_usage},

        .examples =
            {
                "sn pkg add github.com/user/repo",
                "sn pkg remove repo",
                "sn pkg update",
                "sn pkg status",
            },

        .execute = pkg,

        .children = {add_command(), remove_command(), update_command(),
                     status_command()}};

    return command;
}
} // namespace sonata::commands