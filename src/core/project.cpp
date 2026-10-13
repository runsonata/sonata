#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <sonata/core/luau/datafile.hpp>
#include <sonata/core/project.hpp>

namespace fs = std::filesystem;

namespace sonata {

namespace {

// Manifest reading
//
// "where" is a prefix for error messages: the manifest path, optionally
// followed by the location inside it (e.g. "<path>: dependencies[2]").

std::string requireString(
    const luau::DataValue& table,
    const std::string& key,
    const std::string& where
)
{
    const luau::DataValue* field = table.find(key);

    if (!field || field->isNil())
    {
        throw std::runtime_error(
            where + ": missing required field '" + key + "'"
        );
    }

    if (!field->isString())
    {
        throw std::runtime_error(
            where + ": field '" + key + "' must be a string"
        );
    }

    return field->asString();
}

std::string optionalString(
    const luau::DataValue& table,
    const std::string& key,
    std::string fallback,
    const std::string& where
)
{
    const luau::DataValue* field = table.find(key);

    if (!field || field->isNil())
        return fallback;

    if (!field->isString())
    {
        throw std::runtime_error(
            where + ": field '" + key + "' must be a string"
        );
    }

    return field->asString();
}

std::vector<std::string> optionalStringArray(
    const luau::DataValue& table,
    const std::string& key,
    const std::string& where
)
{
    std::vector<std::string> result;

    const luau::DataValue* field = table.find(key);

    if (!field || field->isNil())
        return result;

    if (!field->isArray())
    {
        throw std::runtime_error(
            where + ": field '" + key +
            "' must be a plain array of strings, e.g. { \"a\", \"b\" }"
        );
    }

    for (const auto& item : field->items())
    {
        if (!item.isString())
        {
            throw std::runtime_error(
                where + ": every entry in '" + key + "' must be a string"
            );
        }

        result.push_back(item.asString());
    }

    return result;
}

std::vector<Project::Dependency> optionalDependencies(
    const luau::DataValue& table,
    const std::string& where
)
{
    std::vector<Project::Dependency> result;

    const luau::DataValue* field = table.find("dependencies");

    if (!field || field->isNil())
        return result;

    if (!field->isArray())
    {
        throw std::runtime_error(
            where + ": field 'dependencies' must be a plain array of tables, "
            "e.g. { { name = \"foo\", url = \"https://...\", ref = \"v1.0\" } }"
        );
    }

    std::unordered_set<std::string> seen;
    std::size_t index = 0;

    for (const auto& item : field->items())
    {
        ++index; // Luau arrays are 1-based, so report them that way.

        const std::string entryWhere =
            where + ": dependencies[" + std::to_string(index) + "]";

        if (!item.isTable())
        {
            throw std::runtime_error(
                entryWhere +
                " must be a table with 'name', 'url' and an optional 'ref'"
            );
        }

        Project::Dependency dependency;
        dependency.name = requireString(item, "name", entryWhere);
        dependency.url = requireString(item, "url", entryWhere);
        dependency.ref = optionalString(item, "ref", "", entryWhere);

        if (const auto problem = dependency.validationError())
            throw std::runtime_error(entryWhere + ": " + *problem);

        if (!seen.insert(dependency.name).second)
        {
            throw std::runtime_error(
                entryWhere + ": duplicate dependency name '" +
                dependency.name + "'"
            );
        }

        result.push_back(std::move(dependency));
    }

    return result;
}

// "where: build.<key> must be ..." helpers for the build table.
std::optional<int> optionalLevel(
    const luau::DataValue& build,
    const std::string& key,
    const std::string& where
)
{
    const luau::DataValue* field = build.find(key);

    if (!field || field->isNil())
        return std::nullopt;

    const double value = field->isNumber() ? field->asNumber() : -1.0;

    if (value != 0.0 && value != 1.0 && value != 2.0)
    {
        throw std::runtime_error(
            where + ": build." + key + " must be 0, 1 or 2"
        );
    }

    return static_cast<int>(value);
}

std::optional<bool> optionalBoolean(
    const luau::DataValue& build,
    const std::string& key,
    const std::string& where
)
{
    const luau::DataValue* field = build.find(key);

    if (!field || field->isNil())
        return std::nullopt;

    if (!field->isBoolean())
    {
        throw std::runtime_error(
            where + ": build." + key + " must be true or false"
        );
    }

    return field->asBoolean();
}

// A path inside the project, written relative to its root. "where" names the
// field for the error message.
void checkRelativePath(const std::string& path, const std::string& where)
{
    if (path.empty())
        throw std::runtime_error(where + " must not be empty");

    const fs::path p(path);

    if (p.is_absolute() || p.has_root_name() || p.has_root_directory())
        throw std::runtime_error(where + " must be relative to the project root ('" + path + "')");

    const fs::path normal = p.lexically_normal();

    if (normal.empty() || *normal.begin() == "..")
        throw std::runtime_error(where + " points outside the project ('" + path + "')");

    if (normal == ".")
        throw std::runtime_error(where + " must name a file or folder inside the project, not the project itself");
}

Project::Build optionalBuild(
    const luau::DataValue& table,
    const std::string& where
)
{
    Project::Build build;

    const luau::DataValue* field = table.find("build");

    if (!field || field->isNil())
        return build;

    if (!field->isTable())
        throw std::runtime_error(where + ": field 'build' must be a table");

    const luau::DataValue& b = *field;

    if (const luau::DataValue* output = b.find("output"); output && !output->isNil())
    {
        if (!output->isString())
            throw std::runtime_error(where + ": build.output must be a string");

        build.output = output->asString();
        if (build.output->empty())
            throw std::runtime_error(where + ": build.output must not be empty");
    }

    if (const luau::DataValue* libraries = b.find("libraries"); libraries && !libraries->isNil())
    {
        if (!libraries->isTable() || !libraries->isArray())
        {
            throw std::runtime_error(
                where + ": build.libraries must be a plain array of library names, "
                "e.g. { \"fs\", \"path\" }"
            );
        }

        build.libraries = optionalStringArray(b, "libraries", where + ": build");
    }

    build.assets = optionalStringArray(b, "assets", where + ": build");

    for (const std::string& asset : build.assets)
        checkRelativePath(asset, where + ": build.assets entry");

    build.optimize = optionalLevel(b, "optimize", where);
    build.debug = optionalLevel(b, "debug", where);
    build.compress = optionalBoolean(b, "compress", where);
    build.standalone = optionalBoolean(b, "standalone", where);

    return build;
}

// Manifest writing
void addEntry(
    luau::DataValue::Entries& entries,
    const char* key,
    luau::DataValue value
)
{
    entries.push_back(luau::DataValue::Entry{
        luau::DataValue::Key::ofString(key),
        std::move(value)
    });
}

luau::DataValue stringArray(const std::vector<std::string>& strings)
{
    std::vector<luau::DataValue> values;
    values.reserve(strings.size());

    for (const auto& s : strings)
        values.emplace_back(s);

    return luau::DataValue::array(std::move(values));
}

luau::DataValue dependencyTable(const Project::Dependency& dependency)
{
    luau::DataValue::Entries entries;

    addEntry(entries, "name", dependency.name);
    addEntry(entries, "url", dependency.url);

    if (!dependency.ref.empty())
        addEntry(entries, "ref", dependency.ref);

    return luau::DataValue(std::move(entries));
}

// Keys that saveManifest() regenerates from Manifest. Anything else already
// in the file is carried over untouched.
bool isManagedKey(const std::string& key)
{
    return key == "name" || key == "version" || key == "description" ||
           key == "authors" || key == "entrypoint" || key == "dependencies";
}

// Dependency validation helpers

bool isControlChar(char c) noexcept
{
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7F;
}

bool containsControlChar(const std::string& s) noexcept
{
    return std::any_of(s.begin(), s.end(), isControlChar);
}

bool isAsciiDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// Deliberately ASCII-only and locale-independent (unlike std::isalnum).
bool isIdentifierChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           isAsciiDigit(c) || c == '_';
}

char toLowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// These deliberately don't echo the offending value: it may contain control
// characters, and this text ends up in terminals and logs.

std::optional<std::string> nameError(const std::string& name)
{
    if (name.empty())
        return "name must not be empty";

    if (!std::all_of(name.begin(), name.end(), isIdentifierChar))
        return "name may only contain letters, digits and underscores";

    if (isAsciiDigit(name.front()))
        return "name must not start with a digit";

    return std::nullopt;
}

std::optional<std::string> urlError(const std::string& url)
{
    if (url.empty())
        return "url must not be empty";

    if (url.front() == '-')
        return "url must not start with '-'";

    if (containsControlChar(url))
        return "url must not contain control characters";

    return std::nullopt;
}

std::optional<std::string> refError(const std::string& ref)
{
    // The ref is optional; empty means "not specified".
    if (ref.empty())
        return std::nullopt;

    if (ref.front() == '-')
        return "ref must not start with '-'";

    if (containsControlChar(ref))
        return "ref must not contain control characters";

    if (ref.find("..") != std::string::npos)
        return "ref must not contain '..'";

    if (ref.find_first_of(" ~^:?*[\\") != std::string::npos)
    {
        return "ref must not contain spaces or any of the characters "
               "~ ^ : ? * [ \\";
    }

    return std::nullopt;
}

} // namespace

// Project::Dependency
std::optional<std::string> Project::Dependency::validationError() const {
    if (auto problem = nameError(name))
        return problem;

    if (auto problem = urlError(url))
        return problem;

    return refError(ref);
}

void Project::Dependency::validate() const {
    if (const auto problem = validationError())
        throw std::invalid_argument("Invalid dependency: " + *problem);
}

bool Project::Dependency::sameSource(const Dependency& other) const {
    return ref == other.ref && normalizeUrl(url) == normalizeUrl(other.url);
}

std::string Project::Dependency::normalizeUrl(std::string url) {
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    while (!url.empty() && isSpace(url.back())) {
        url.pop_back();
    }
    url.erase(url.begin(), std::find_if_not(url.begin(), url.end(), isSpace));

    const auto stripTrailingSlashes = [&url] {
        while (!url.empty() && url.back() == '/') {
            url.pop_back();
        }
    };

    stripTrailingSlashes();

    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0) {
        url.resize(url.size() - 4);
        stripTrailingSlashes();
    }

    // The scheme and host are case-insensitive. The path (and any
    // "user@" part) is not, so those are left alone.
    const std::size_t schemeEnd = url.find("://");

    if (schemeEnd != std::string::npos) {
        const std::size_t authorityStart = schemeEnd + 3;

        std::size_t authorityEnd = url.find('/', authorityStart);
        if (authorityEnd == std::string::npos) {
            authorityEnd = url.size();
        }

        // Skip over "user[:password]@" if there is one.
        std::size_t hostStart = authorityStart;
        const std::size_t at = url.find('@', authorityStart);
        if (at != std::string::npos && at < authorityEnd) {
            hostStart = at + 1;
        }

        for (std::size_t i = 0; i < schemeEnd; ++i) {
            url[i] = toLowerAscii(url[i]);
        }

        for (std::size_t i = hostStart; i < authorityEnd; ++i) {
            url[i] = toLowerAscii(url[i]);
        }
    }

    return url;
}

// Project
Project::Project(fs::path root)
    : root_(fs::absolute(std::move(root)).lexically_normal()),
      sonataDir_(root_ / ".sonata"),
      projectFile_(sonataDir_ / "project.luau"),
      dependenciesDir_(sonataDir_ / "deps") {
}

Project Project::open(const fs::path& root) {
    Project project(root);
    project.validate();

    return project;
}

Project Project::find(const fs::path& start) {
    fs::path current = fs::absolute(start).lexically_normal();

    // If the supplied path is a file, begin at its parent.
    if (fs::exists(current) && fs::is_regular_file(current)) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        const fs::path sonataDir = current / ".sonata";
        const fs::path projectFile = sonataDir / "project.luau";

        // The entrypoint filename is configurable (via project.luau), so it
        // can't be part of this detection heuristic; the manifest itself
        // is the one thing every Sonata project is guaranteed to have.
        if (fs::is_directory(sonataDir) &&
            fs::is_regular_file(projectFile)) {
            return Project::open(current);
        }

        const fs::path parent = current.parent_path();

        // We reached the filesystem root.
        if (parent == current) {
            break;
        }

        current = parent;
    }

    throw std::runtime_error(
        "Could not find a Sonata project from: " + start.string()
    );
}

void Project::validate() {
    if (!fs::exists(root_)) {
        throw std::runtime_error(
            "Project directory does not exist: " + root_.string()
        );
    }

    if (!fs::is_directory(root_)) {
        throw std::runtime_error(
            "Project path is not a directory: " + root_.string()
        );
    }

    if (!fs::exists(sonataDir_)) {
        throw std::runtime_error(
            "Project is missing the sonata directory: " + sonataDir_.string()
        );
    }

    if (!fs::is_directory(sonataDir_)) {
        throw std::runtime_error(
            "sonata is not a directory: " + sonataDir_.string()
        );
    }

    if (!fs::exists(projectFile_)) {
        throw std::runtime_error(
            "Project is missing project.luau: " + projectFile_.string()
        );
    }

    if (!fs::is_regular_file(projectFile_)) {
        throw std::runtime_error(
            "project.luau is not a regular file: " + projectFile_.string()
        );
    }

    if (fs::exists(dependenciesDir_) && !fs::is_directory(dependenciesDir_)) {
        throw std::runtime_error(
            "deps is not a directory: " + dependenciesDir_.string()
        );
    }

    // The .sonata/deps/ folder is optional.
    //
    // project/
    // ├── main.luau
    // └── .sonata/
    //
    // It will simply report false through the corresponding
    // hasDependenciesDirectory* function.

    loadManifest();
}

void Project::loadManifest() {
    const std::string where = projectFile_.string();

    luau::DataValue table;
    std::string source;

    try {
        std::ifstream file(projectFile_, std::ios::binary);
        if (!file)
            throw std::runtime_error("could not open the file");

        std::ostringstream contents;
        contents << file.rdbuf();
        source = contents.str();

        table = luau::DataFile::parse(source, "=" + where);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Failed to parse " + where + ": " + e.what()
        );
    }

    if (!table.isTable()) {
        throw std::runtime_error(where + " must return a table");
    }

    manifest_.name = requireString(table, "name", where);
    manifest_.version = optionalString(table, "version", "0.0.0", where);
    manifest_.description = optionalString(table, "description", "", where);
    manifest_.authors = optionalStringArray(table, "authors", where);
    manifest_.dependencies = optionalDependencies(table, where);
    manifest_.entrypoint = optionalString(table, "entrypoint", "main.luau", where);
    manifest_.build = optionalBuild(table, where);

    const std::vector<std::string> directives = luau::DataFile::directives(source);
    manifest_.strict = std::find(directives.begin(), directives.end(), "strict") != directives.end();

    // Resolve + confine the entrypoint to the project root: project.luau
    // shouldn't be able to point outside of it (e.g. "../../etc/passwd").
    const fs::path candidate = (root_ / manifest_.entrypoint).lexically_normal();
    const fs::path relative = candidate.lexically_relative(root_);

    const bool escapesRoot = relative.empty() ||
        (relative.begin() != relative.end() && *relative.begin() == "..");

    if (escapesRoot) {
        throw std::runtime_error(
            where + ": entrypoint '" + manifest_.entrypoint +
            "' resolves outside the project root"
        );
    }

    entrypoint_ = candidate;

    if (!fs::exists(entrypoint_)) {
        throw std::runtime_error(
            "Project is missing its entrypoint: " + entrypoint_.string()
        );
    }

    if (!fs::is_regular_file(entrypoint_)) {
        throw std::runtime_error(
            "entrypoint is not a regular file: " + entrypoint_.string()
        );
    }
}

void Project::saveManifest() const {
    // Never write something open() would refuse to load.
    std::unordered_set<std::string> names;

    for (const Dependency& dependency : manifest_.dependencies) {
        dependency.validate();

        if (!names.insert(dependency.name).second) {
            throw std::invalid_argument(
                "Duplicate dependency name: " + dependency.name
            );
        }
    }

    luau::DataValue::Entries entries;

    addEntry(entries, "name", manifest_.name);
    addEntry(entries, "version", manifest_.version);

    if (!manifest_.description.empty()) {
        addEntry(entries, "description", manifest_.description);
    }

    if (!manifest_.authors.empty()) {
        addEntry(entries, "authors", stringArray(manifest_.authors));
    }

    addEntry(entries, "entrypoint", manifest_.entrypoint);

    if (!manifest_.dependencies.empty()) {
        std::vector<luau::DataValue> items;
        items.reserve(manifest_.dependencies.size());

        for (const Dependency& dependency : manifest_.dependencies) {
            items.push_back(dependencyTable(dependency));
        }

        addEntry(entries, "dependencies", luau::DataValue::array(std::move(items)));
    }

    // Carry over anything in the existing file that we don't manage, so
    // saving doesn't silently drop keys other tools (or the user) added.
    if (fs::exists(projectFile_)) {
        luau::DataValue existing;

        try {
            existing = luau::DataFile::parseFile(projectFile_);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Refusing to overwrite " + projectFile_.string() +
                ", it can't be parsed: " + e.what()
            );
        }

        for (const auto& entry : existing.asTable()) {
            const bool managed =
                entry.key &&
                entry.key->kind == luau::DataValue::Key::Kind::String &&
                isManagedKey(entry.key->string);

            if (!managed) {
                entries.push_back(entry);
            }
        }
    }

    const luau::DataValue table(std::move(entries));

    // Write to a sibling temp file and rename over the real one, so a
    // failure part-way through can't leave a truncated manifest behind.
    fs::path temp = projectFile_;
    temp += ".tmp";

    try {
        {
            std::ofstream file(temp, std::ios::binary);
            if (!file)
                throw std::runtime_error("could not open for writing: " + temp.string());

            if (manifest_.strict)
                file << "--!strict\n";

            file << luau::DataFile::serialize(table);

            if (!file)
                throw std::runtime_error("could not write " + temp.string());
        }
        fs::rename(temp, projectFile_);
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove(temp, ignored);

        throw std::runtime_error(
            "Failed to write " + projectFile_.string() + ": " + e.what()
        );
    }
}

const Project::Dependency* Project::findDependency(
    const std::string& name
) const noexcept {
    const auto it = std::find_if(
        manifest_.dependencies.begin(),
        manifest_.dependencies.end(),
        [&](const Dependency& d) { return d.name == name; }
    );

    return it == manifest_.dependencies.end() ? nullptr : &*it;
}

bool Project::addDependency(Dependency dependency) {
    dependency.validate();

    auto& deps = manifest_.dependencies;
    const std::vector<Dependency> previous = deps;

    const auto it = std::find_if(
        deps.begin(), deps.end(),
        [&](const Dependency& d) { return d.name == dependency.name; }
    );

    const bool replaced = it != deps.end();

    if (replaced) {
        *it = std::move(dependency);
    } else {
        deps.push_back(std::move(dependency));
    }

    try {
        saveManifest();
    } catch (...) {
        deps = previous;
        throw;
    }

    return replaced;
}

bool Project::removeDependency(const std::string& name) {
    auto& deps = manifest_.dependencies;

    const auto it = std::find_if(
        deps.begin(), deps.end(),
        [&](const Dependency& d) { return d.name == name; }
    );

    if (it == deps.end()) {
        return false;
    }

    const std::vector<Dependency> previous = deps;
    deps.erase(it);

    try {
        saveManifest();
    } catch (...) {
        deps = previous;
        throw;
    }

    return true;
}

const fs::path& Project::root() const noexcept {
    return root_;
}

const fs::path& Project::entrypoint() const noexcept {
    return entrypoint_;
}

const fs::path& Project::sonataDir() const noexcept {
    return sonataDir_;
}

const fs::path& Project::projectFile() const noexcept {
    return projectFile_;
}

const fs::path& Project::dependenciesDir() const noexcept {
    return dependenciesDir_;
}

const Project::Manifest& Project::manifest() const noexcept {
    return manifest_;
}

Project::Manifest& Project::manifest() noexcept {
    return manifest_;
}

bool Project::hasDependenciesDirectory() const noexcept {
    return fs::is_directory(dependenciesDir_);
}

std::vector<fs::path> Project::sourceFiles() const {
    std::vector<fs::path> files;

    if (!fs::exists(root_)) {
        return files;
    }

    for (auto it = fs::recursive_directory_iterator(root_);
         it != fs::recursive_directory_iterator();
         ++it) {
        const fs::directory_entry& entry = *it;

        // Don't search inside .sonata/.
        //
        // This is important because dependencies may themselves contain
        // Luau files, and those shouldn't automatically become part of
        // the project's source tree.
        //
        // (Merely `continue`-ing isn't enough: the iterator would still
        // descend into the directory on the next increment.)
        if (entry.is_directory() && entry.path() == sonataDir_) {
            it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() != ".luau") {
            continue;
        }

        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end());

    return files;
}

} // namespace sonata