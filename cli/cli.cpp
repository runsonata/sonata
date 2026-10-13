#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "cli.hpp"
#include "commands/build.hpp"
#include "commands/init.hpp"
#include "commands/pkg.hpp"
#include "commands/run.hpp"
#include <sonata/core/info.hpp>

namespace sonata::cli {
namespace {
namespace ui {

enum class Stream { Out, Err };

namespace code {
constexpr std::string_view dim = "2";
constexpr std::string_view cyan = "36";
constexpr std::string_view bold_cyan = "1;36";
constexpr std::string_view bold_red = "1;31";
constexpr std::string_view bold_yellow = "1;33";
} // namespace code

#ifdef _WIN32
bool enable_vt(DWORD handle_id) {
    HANDLE h = GetStdHandle(handle_id);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode))
        return false;

    return SetConsoleMode(h,
                          mode | 0x0004); // ENABLE_VIRTUAL_TERMINAL_PROCESSING
}
#endif

bool color_enabled(Stream stream) {
    struct State {
        bool out;
        bool err;
    };

    static const State state = [] {
        State s{false, false};

        const char *no_color = std::getenv("NO_COLOR");
        if (no_color && *no_color)
            return s;

        const char *force = std::getenv("FORCE_COLOR");
        if (force && *force) {
            s.out = s.err = true;
            return s;
        }

        const char *term = std::getenv("TERM");
        if (term && std::string_view(term) == "dumb")
            return s;

#ifdef _WIN32
        s.out = _isatty(_fileno(stdout)) && enable_vt(STD_OUTPUT_HANDLE);
        s.err = _isatty(_fileno(stderr)) && enable_vt(STD_ERROR_HANDLE);
#else
        s.out = isatty(STDOUT_FILENO);
        s.err = isatty(STDERR_FILENO);
#endif
        return s;
    }();

    return stream == Stream::Out ? state.out : state.err;
}

std::string paint(std::string_view text, std::string_view style,
                  Stream stream = Stream::Out) {
    if (!color_enabled(stream))
        return std::string(text);

    std::string out = "\x1b[";
    out += style;
    out += 'm';
    out += text;
    out += "\x1b[0m";
    return out;
}

// Semantic helpers (stdout)
std::string dim(std::string_view s) { return paint(s, code::dim); }
std::string accent(std::string_view s) { return paint(s, code::bold_cyan); }
std::string heading(std::string_view s) { return paint(s, code::bold_yellow); }

// "error:" label for stderr
std::string error_label() {
    return paint("error:", code::bold_red, Stream::Err);
}

std::string pad(std::string_view s, std::size_t width) {
    std::string out(s);
    if (out.size() < width)
        out.append(width - out.size(), ' ');
    return out;
}

struct Row {
    std::string label;
    std::string text;
};

// Prints rows as two aligned columns:   "  label   text"
void print_rows(std::ostream &os, const std::vector<Row> &rows,
                std::string_view label_style = code::cyan) {
    std::size_t width = 0;
    for (const auto &row : rows)
        width = std::max(width, row.label.size());

    for (const auto &row : rows) {
        os << "  " << paint(pad(row.label, width), label_style) << "   "
           << row.text << '\n';
    }
}

void section(std::ostream &os, std::string_view title) {
    os << heading(title) << '\n';
}

} // namespace ui

using ui::Row;

const std::vector<Command> &commands() {
    static const std::vector<Command> commands = {
        commands::init_command(),
        commands::pkg_command(),
        commands::run_command(),
        commands::build_command()
    };

    return commands;
}

const Command *find_command(const std::vector<Command> &commands,
                            const std::string &name) {
    for (const auto &command : commands) {
        if (command.name == name)
            return &command;
    }

    return nullptr;
}

// subcommands and help flags

bool is_help_flag(const std::string &s) { return s == "-h" || s == "--help"; }

// Child names may carry usage text ("add [path] [deps]"); the name
// proper is the first word.
std::string first_word(const std::string &s) {
    return s.substr(0, s.find(' '));
}

const Command *find_child(const Command &parent, const std::string &name) {
    for (const auto &child : parent.children) {
        if (first_word(child.name) == name)
            return &child;
    }

    return nullptr;
}

// True if a help flag appears before a bare "--".
bool has_help_flag(int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        const std::string token = argv[i];

        if (token == "--")
            return false;

        if (is_help_flag(token))
            return true;
    }

    return false;
}

bool declares_help(const Command &command) {
    for (const auto &argument : command.args) {
        if (argument.name.find("--help") != std::string::npos)
            return true;

        for (const auto &alias : argument.aliases) {
            if (is_help_flag(alias))
                return true;
        }
    }

    return false;
}

// "did you mean" support
// yes i used ai for this i started c++ 3 weeks ago im not writing ts
std::size_t edit_distance(const std::string &a, const std::string &b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);

    for (std::size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;

    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;

        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] =
                std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }

        std::swap(prev, cur);
    }

    return prev[b.size()];
}

const Command *suggest_command(const std::string &input) {
    const Command *best = nullptr;
    std::size_t best_distance = 3;

    for (const auto &command : commands()) {
        const std::size_t d = edit_distance(input, command.name);

        if (d < best_distance) {
            best_distance = d;
            best = &command;
        }
    }

    return best;
}

// printing

void print_banner() {
    std::cout << ui::accent(info::name) << ' '
              << ui::dim("v" + info::version_string());

    if (info::is_debug)
        std::cout << ' ' << ui::accent(info::build_type);

    std::cout << '\n' << ui::dim(info::tagline) << "\n\n";
}

void print_version_verbose() {
    std::cout << ui::accent(info::name) << ' ' << info::version_string()
              << "\n\n";

    std::vector<Row> rows;

    if (!info::git_hash.empty())
        rows.push_back({"commit", std::string(info::git_hash)});

    rows.push_back({"build", std::string(info::build_type)});
    rows.push_back({"platform", info::platform_string()});
    rows.push_back({"compiler", info::compiler_string()});

    if (!info::luau_version.empty())
        rows.push_back({"luau", std::string(info::luau_version)});

    ui::print_rows(std::cout, rows, ui::code::dim);
}

// `prefix` is the path leading to this command, e.g. "sn" or "sn pkg".
void print_command_help(const Command &command, const std::string &prefix) {
    std::cout << ui::accent(prefix + ' ' + command.name) << '\n'
              << command.description << "\n\n";

    if (!command.usage.empty()) {
        ui::section(std::cout, "Usage");

        for (const auto &usage : command.usage)
            std::cout << "  " << usage << '\n';

        std::cout << '\n';
    }

    {
        ui::section(std::cout, "Arguments");

        std::vector<Row> rows;

        for (const auto &argument : command.args) {
            std::string label = argument.name;

            if (!argument.aliases.empty()) {
                label += " (";

                for (std::size_t i = 0; i < argument.aliases.size(); ++i) {
                    if (i > 0)
                        label += ", ";

                    label += argument.aliases[i];
                }

                label += ')';
            }

            rows.push_back({std::move(label), argument.description});
        }

        // Every command gets -h/--help from the CLI itself; skip it
        // only if the command still declares its own.
        if (!declares_help(command))
            rows.push_back({"--help, -h", "Show this help"});

        ui::print_rows(std::cout, rows);
        std::cout << '\n';
    }

    if (!command.children.empty()) {
        ui::section(std::cout, "Commands");

        std::vector<Row> rows;

        for (const auto &child : command.children)
            rows.push_back({child.name, child.description});

        ui::print_rows(std::cout, rows);
        std::cout << '\n';
    }

    if (!command.examples.empty()) {
        ui::section(std::cout, "Examples");

        for (const auto &example : command.examples)
            std::cout << "  " << ui::dim("$") << ' ' << example << '\n';

        std::cout << '\n';
    }
}

void print_help() {
    print_banner();

    ui::section(std::cout, "Usage");
    std::cout << "  " << info::binary << " <command> [arguments]\n\n";

    ui::section(std::cout, "Commands");

    std::vector<Row> command_rows;

    for (const auto &command : commands())
        command_rows.push_back({command.name, command.description});

    ui::print_rows(std::cout, command_rows);
    std::cout << '\n';

    ui::section(std::cout, "Options");

    ui::print_rows(std::cout, {{"-h, --help", "Show help"},
                               {"-v, --version", "Show version"}});

    std::cout << '\n'
              << ui::dim("Run '" + std::string(info::binary) +
                         " <command> --help' for details on a command.")
              << '\n';
}

} // anonymous namespace

int CLI::run(int argc, char **argv) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    const std::string first = argv[1];

    if (first == "--help" || first == "-h") {
        print_help();
        return 0;
    }

    if (first == "--version" || first == "-v") {
        // Plain one-liner by default so scripts can parse it;
        // "sn --version --verbose" prints full build details.
        if (argc >= 3 && std::string(argv[2]) == "--verbose") {
            print_version_verbose();
        } else {
            std::cout << info::id << ' ' << info::version_string() << '\n';
        }

        return 0;
    }

    const Command *command = find_command(commands(), first);

    if (!command) {
        std::cerr << ui::error_label() << " unknown command '" << first
                  << "'\n";

        if (const Command *suggestion = suggest_command(first)) {
            std::cerr << "  Did you mean '"
                      << ui::paint(suggestion->name, ui::code::cyan,
                                   ui::Stream::Err)
                      << "'?\n";
        }

        std::cerr << "\nrun 'sn --help' to see a list of all available commands\n";

        return 1;
    }

    // Walk down to the deepest subcommand named on the command line,
    // e.g. "sn pkg add ..." -> pkg -> add.
    const Command *deepest = command;
    std::string prefix(info::binary);
    int consumed = 2;

    while (consumed < argc) {
        const Command *child = find_child(*deepest, argv[consumed]);

        if (!child)
            break;

        prefix += ' ' + first_word(deepest->name);
        deepest = child;
        ++consumed;
    }

    // Help is handled here for every command, so commands never have
    // to look for -h/--help themselves.
    if (has_help_flag(argc - consumed, argv + consumed)) {
        print_command_help(*deepest, prefix);
        return 0;
    }

    // Commands receive everything after their own name and dispatch to
    // their subcommands themselves.
    if (command->execute) {
        return command->execute(argc - 2, argv + 2);
    }

    if (deepest->execute) {
        return deepest->execute(argc - consumed, argv + consumed);
    }

    print_command_help(*deepest, prefix);
    return 0;
}

} // namespace sonata::cli