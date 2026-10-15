#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sonata/core/luau/datafile.hpp>
#include "lsp.hpp"

namespace sonata::lsp {

// The contents of types/, compiled in (see cli/CMakeLists.txt).
struct TypeFile {
    const char *path; // relative to types/, with '/' separators
    const unsigned char *data;
    std::size_t size;
};

extern const TypeFile type_files[];
extern const std::size_t type_file_count;

namespace {

namespace fs = std::filesystem;

constexpr std::string_view types_dir = ".sonata/types";
constexpr std::string_view libs_alias = ".sonata/types/libs";
constexpr std::string_view globals_file = ".sonata/types/globals.d.luau";
constexpr std::string_view deps_dir = ".sonata/deps";
constexpr std::string_view manifest_file = ".sonata/project.luau";

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

// Luau compares alias names case-insensitively.
bool same_alias(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](unsigned char x, unsigned char y) {
                          return std::tolower(x) == std::tolower(y);
                      });
}

// "./a/b/" -> "a/b", so alias paths can be compared however they're written.
std::string tidy_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (starts_with(path, "./"))
        path.erase(0, 2);
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
//
// Just enough JSON to merge into .luaurc and .vscode/settings.json while
// keeping everything else in them. Objects keep their key order and numbers
// their original text. The comments and trailing commas VS Code allows are
// accepted, but comments can't be written back, so a file that has them is
// never rewritten (see update_json_file).

struct Json {
    enum class Kind { Null, Boolean, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text; // String: the value. Number: the literal.
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    static Json string(std::string_view value) {
        Json json;
        json.kind = Kind::String;
        json.text = value;
        return json;
    }

    static Json boolean_value(bool value) {
        Json json;
        json.kind = Kind::Boolean;
        json.boolean = value;
        return json;
    }

    static Json object() {
        Json json;
        json.kind = Kind::Object;
        return json;
    }

    Json *find(std::string_view key) {
        for (auto &[name, value] : members) {
            if (name == key)
                return &value;
        }
        return nullptr;
    }

    // Replaces the value of "key" where it is, or appends it.
    Json &set(std::string_view key, Json value) {
        if (Json *existing = find(key)) {
            *existing = std::move(value);
            return *existing;
        }
        members.emplace_back(std::string(key), std::move(value));
        return members.back().second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {
        if (starts_with(text_, "\xEF\xBB\xBF")) // UTF-8 byte order mark
            pos_ = 3;
    }

    Json parse() {
        Json json = value(0);
        skip_space();
        if (pos_ < text_.size())
            fail("unexpected text after the end of the value");
        return json;
    }

    bool had_comments() const { return comments_; }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
    bool comments_ = false;

    [[noreturn]] void fail(const std::string &message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw std::runtime_error(message + " (line " + std::to_string(line) +
                                 ", column " + std::to_string(column) + ")");
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (text_.compare(pos_, 2, "//") == 0) {
                comments_ = true;
                pos_ = std::min(text_.find('\n', pos_), text_.size());
            } else if (text_.compare(pos_, 2, "/*") == 0) {
                comments_ = true;
                const std::size_t end = text_.find("*/", pos_ + 2);
                if (end == std::string_view::npos)
                    fail("unterminated comment");
                pos_ = end + 2;
            } else {
                break;
            }
        }
    }

    bool consume(std::string_view literal) {
        if (text_.compare(pos_, literal.size(), literal) != 0)
            return false;
        pos_ += literal.size();
        return true;
    }

    Json value(int depth) {
        if (depth > 128)
            fail("nested too deeply");

        skip_space();
        if (pos_ >= text_.size())
            fail("unexpected end of the file");

        const char c = text_[pos_];
        if (c == '{')
            return object(depth);
        if (c == '[')
            return array(depth);
        if (c == '"')
            return Json::string(string());
        if (consume("true"))
            return Json::boolean_value(true);
        if (consume("false"))
            return Json::boolean_value(false);
        if (consume("null"))
            return Json{};
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return number();

        fail(std::string("unexpected character '") + c + "'");
    }

    Json object(int depth) {
        Json json = Json::object();
        ++pos_; // '{'
        while (true) {
            skip_space();
            if (consume("}"))
                return json;
            if (pos_ >= text_.size() || text_[pos_] != '"')
                fail("expected a key in quotes");

            std::string key = string();
            skip_space();
            if (!consume(":"))
                fail("expected ':' after a key");
            json.set(key, value(depth + 1));

            skip_space();
            if (consume(","))
                continue; // a trailing comma ends up at the '}' check above
            if (consume("}"))
                return json;
            fail("expected ',' or '}'");
        }
    }

    Json array(int depth) {
        Json json;
        json.kind = Json::Kind::Array;
        ++pos_; // '['
        while (true) {
            skip_space();
            if (consume("]"))
                return json;
            json.items.push_back(value(depth + 1));

            skip_space();
            if (consume(","))
                continue;
            if (consume("]"))
                return json;
            fail("expected ',' or ']'");
        }
    }

    Json number() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() &&
               std::string_view("+-.eE0123456789").find(text_[pos_]) !=
                   std::string_view::npos)
            ++pos_;

        Json json;
        json.kind = Json::Kind::Number;
        json.text = text_.substr(start, pos_ - start);
        return json;
    }

    unsigned hex4() {
        if (pos_ + 4 > text_.size())
            fail("incomplete \\u escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= c - '0';
            else if (c >= 'a' && c <= 'f')
                value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                value |= c - 'A' + 10;
            else
                fail("invalid \\u escape");
        }
        return value;
    }

    static void append_utf8(std::string &out, std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::string string() {
        ++pos_; // '"'
        std::string out;
        while (true) {
            if (pos_ >= text_.size())
                fail("unterminated string");

            const char c = text_[pos_++];
            if (c == '"')
                return out;
            if (c != '\\') {
                out += c;
                continue;
            }

            if (pos_ >= text_.size())
                fail("unterminated string");
            switch (const char e = text_[pos_++]) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t code = hex4();
                if (code >= 0xD800 && code <= 0xDBFF && consume("\\u")) {
                    const std::uint32_t low = hex4();
                    if (low >= 0xDC00 && low <= 0xDFFF)
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, code);
                break;
            }
            default:
                fail(std::string("invalid escape '\\") + e + "'");
            }
        }
    }
};

void write_json_string(std::string &out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char digits[] = "0123456789abcdef";
                out += "\\u00";
                out += digits[(c >> 4) & 0xF];
                out += digits[c & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void write_json(std::string &out, const Json &json, int depth) {
    const auto newline = [&out](int level) {
        out += '\n';
        out.append(static_cast<std::size_t>(level) * 4, ' ');
    };

    switch (json.kind) {
    case Json::Kind::Null:
        out += "null";
        break;
    case Json::Kind::Boolean:
        out += json.boolean ? "true" : "false";
        break;
    case Json::Kind::Number:
        out += json.text;
        break;
    case Json::Kind::String:
        write_json_string(out, json.text);
        break;
    case Json::Kind::Array:
        if (json.items.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < json.items.size(); ++i) {
            newline(depth + 1);
            write_json(out, json.items[i], depth + 1);
            if (i + 1 < json.items.size())
                out += ',';
        }
        newline(depth);
        out += ']';
        break;
    case Json::Kind::Object:
        if (json.members.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < json.members.size(); ++i) {
            newline(depth + 1);
            write_json_string(out, json.members[i].first);
            out += ": ";
            write_json(out, json.members[i].second, depth + 1);
            if (i + 1 < json.members.size())
                out += ',';
        }
        newline(depth);
        out += '}';
        break;
    }
}

std::string to_json(const Json &json) {
    std::string out;
    write_json(out, json, 0);
    out += '\n';
    return out;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::optional<std::string> read_file(const fs::path &path) {
    std::error_code ec;
    if (!fs::exists(path, ec))
        return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("could not read " + path.generic_string());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_file(const fs::path &path, std::string_view contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        throw std::runtime_error("could not create " +
                                 path.parent_path().generic_string() + ": " +
                                 ec.message());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out)
        throw std::runtime_error("could not write " + path.generic_string());
}

/*
 * Applies "update" to the JSON object in the file at "path" (an empty one if
 * the file doesn't exist) and writes the result back if anything changed.
 * Returns false, writing nothing, if the change would drop comments from the
 * file: the caller then tells the user what to add by hand.
 */
bool update_json_file(const fs::path &path,
                      const std::function<void(Json &)> &update) {
    const std::optional<std::string> text = read_file(path);

    Json json = Json::object();
    bool comments = false;
    if (text) {
        try {
            JsonParser parser(*text);
            json = parser.parse();
            comments = parser.had_comments();
        } catch (const std::runtime_error &e) {
            throw std::runtime_error("could not parse " + path.generic_string() +
                                     ": " + e.what());
        }
        if (json.kind != Json::Kind::Object)
            throw std::runtime_error(path.generic_string() +
                                     " doesn't hold a JSON object");
    }

    const std::string before = to_json(json);
    update(json);
    const std::string after = to_json(json);

    if (text && before == after)
        return true;
    if (comments)
        return false;

    write_file(path, after);
    return true;
}

// ---------------------------------------------------------------------------
// Packages and aliases
// ---------------------------------------------------------------------------

// The rule for dependency names (see Project::Dependency).
bool is_package_name(std::string_view name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

std::vector<std::string> installed_packages(const fs::path &root) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(root / deps_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::string name = it->path().filename().string();
        std::error_code file_ec;
        if (is_package_name(name) &&
            fs::is_regular_file(it->path() / manifest_file, file_ec))
            names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

/*
 * Where "@name" should point for the package in .sonata/deps/<name>.
 *
 * Sonata loads a package's manifest entrypoint, or init.luau without one (see
 * src/core/module.cpp). luau-lsp only knows init.luau, so for any other
 * entrypoint the alias points at the entrypoint itself: "@name" then
 * resolves, though "@name/file" is looked for next to the entrypoint rather
 * than at the package root.
 */
std::string package_alias(const fs::path &root, const std::string &name) {
    const std::string package = std::string(deps_dir) + "/" + name;

    std::string entrypoint;
    try {
        const luau::DataValue manifest =
            luau::DataFile::parseFile(root / package / manifest_file);
        const luau::DataValue *value = manifest.find("entrypoint");
        if (value == nullptr || !value->isString())
            return package;
        entrypoint = tidy_path(value->asString());
    } catch (const std::exception &) {
        return package;
    }

    std::error_code ec;
    const fs::path entry(entrypoint);
    const bool inside =
        !entry.is_absolute() && !entry.has_root_name() &&
        std::none_of(entry.begin(), entry.end(),
                     [](const fs::path &part) { return part == ".."; });
    if (!inside || !ends_with(entrypoint, ".luau") ||
        !fs::is_regular_file(root / package / entry, ec))
        return package;

    std::string stem = entrypoint.substr(0, entrypoint.size() - 5);
    if (stem == "init")
        return package;
    if (ends_with(stem, "/init"))
        stem.resize(stem.size() - 5);
    return package + "/" + stem;
}

bool is_package_alias(const Json &value) {
    return value.kind == Json::Kind::String &&
           starts_with(tidy_path(value.text), std::string(deps_dir) + "/");
}

// Sets an alias, replacing one with the same name (in any case) where it is.
void set_alias(Json &aliases, std::string_view name, std::string_view path) {
    for (auto &[key, value] : aliases.members) {
        if (same_alias(key, name)) {
            key = name;
            value = Json::string(path);
            return;
        }
    }
    aliases.set(name, Json::string(path));
}

void update_aliases(Json &config, const fs::path &root) {
    Json *aliases = config.find("aliases");
    if (aliases == nullptr || aliases->kind == Json::Kind::Null)
        aliases = &config.set("aliases", Json::object());
    else if (aliases->kind != Json::Kind::Object)
        throw std::runtime_error("\"aliases\" in .luaurc isn't an object");

    const std::vector<std::string> packages = installed_packages(root);
    const auto installed = [&packages](std::string_view alias) {
        return std::any_of(packages.begin(), packages.end(),
                           [alias](const std::string &name) {
                               return same_alias(name, alias);
                           });
    };

    // Packages that are gone.
    auto &members = aliases->members;
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [&](const auto &member) {
                                     return is_package_alias(member.second) &&
                                            !installed(member.first);
                                 }),
                  members.end());

    set_alias(*aliases, "sonata", libs_alias);
    for (const auto &name : packages)
        set_alias(*aliases, name, package_alias(root, name));
}

void apply_vscode_settings(Json &settings) {
    // Not Roblox, and so no Rojo sourcemap either.
    settings.set("luau-lsp.platform.type", Json::string("standard"));
    settings.set("luau-lsp.sourcemap.enabled", Json::boolean_value(false));

    Json *definitions = settings.find("luau-lsp.types.definitionFiles");
    if (definitions == nullptr || definitions->kind == Json::Kind::Null)
        definitions =
            &settings.set("luau-lsp.types.definitionFiles", Json::object());
    else if (definitions->kind != Json::Kind::Object)
        throw std::runtime_error(
            "\"luau-lsp.types.definitionFiles\" isn't an object (luau-lsp "
            "now expects { \"@name\": \"path\" })");
    definitions->set("@sonata", Json::string(globals_file));
}

// What apply_vscode_settings() adds, for users to copy by hand.
std::string vscode_settings_snippet() {
    Json settings = Json::object();
    apply_vscode_settings(settings);
    return to_json(settings);
}

void write_types(const fs::path &root) {
    const fs::path directory = root / types_dir;

    // Sonata owns this folder: start over, so nothing stale stays behind.
    std::error_code ec;
    fs::remove_all(directory, ec);
    if (ec)
        throw std::runtime_error("could not remove " +
                                 directory.generic_string() + ": " +
                                 ec.message());

    for (std::size_t i = 0; i < type_file_count; ++i) {
        const TypeFile &file = type_files[i];
        write_file(directory / fs::path(file.path),
                   std::string_view(reinterpret_cast<const char *>(file.data),
                                    file.size));
    }
}

} // namespace

void setup(const fs::path &root, const SetupOptions &options,
           std::ostream &out) {
    write_types(root);

    if (!update_json_file(root / ".luaurc", [&root](Json &config) {
            update_aliases(config, root);
        })) {
        Json config = Json::object();
        update_aliases(config, root);
        out << "note: .luaurc has comments, which Sonata can't keep, so it "
               "was left alone. Add these aliases to it:\n"
            << to_json(*config.find("aliases"));
    }

    if (!options.vscode)
        return;

    const fs::path settings = root / ".vscode" / "settings.json";
    try {
        if (update_json_file(settings, apply_vscode_settings))
            return;
        out << "note: " << settings.generic_string()
            << " has comments, which Sonata can't keep, so it was left "
               "alone.";
    } catch (const std::runtime_error &e) {
        out << "note: " << e.what() << ".";
    }
    out << " Add these settings to it:\n" << vscode_settings_snippet();
}

bool is_enabled(const fs::path &root) {
    try {
        const std::optional<std::string> text = read_file(root / ".luaurc");
        if (!text)
            return false;

        Json config = JsonParser(*text).parse();
        if (config.kind != Json::Kind::Object)
            return false;
        Json *aliases = config.find("aliases");
        if (aliases == nullptr || aliases->kind != Json::Kind::Object)
            return false;

        for (const auto &[name, value] : aliases->members) {
            if (same_alias(name, "sonata"))
                return value.kind == Json::Kind::String &&
                       tidy_path(value.text) == libs_alias;
        }
    } catch (const std::exception &) {
    }
    return false;
}

void sync_aliases(const fs::path &root) {
    if (!update_json_file(root / ".luaurc", [&root](Json &config) {
            update_aliases(config, root);
        }))
        throw std::runtime_error(
            ".luaurc has comments, which Sonata can't keep; run 'sn init "
            "--lsp-only' to see the aliases to add");
}

void refresh(const fs::path &root, std::ostream &err) {
    if (!is_enabled(root))
        return;

    try {
        sync_aliases(root);
    } catch (const std::exception &e) {
        err << "warning: couldn't update the package aliases in .luaurc: "
            << e.what() << "\n";
    }
}

} // namespace sonata::lsp
