#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sonata {

class Project {
public:
    /*
     * One entry of the manifest's "dependencies" array. In project.luau:
     *
     *     dependencies = {
     *         { name = "json", url = "https://github.com/acme/json.git", ref = "v1.2.0" },
     *         { name = "util", url = "https://github.com/acme/util.git" },
     *     },
     *
     * Rules (checked by validate()/validationError(), and enforced by both
     * the manifest loader and saveManifest()):
     *
     *   name  Letters, digits and underscores only; must not start with a
     *         digit.
     *   url   Git repository URL. Must not be empty, must not start with
     *         '-', and must not contain control characters.
     *   ref   Optional (empty = not specified): a tag, branch or commit
     *         hash. Must not start with '-', contain control characters,
     *         contain "..", or contain a space or any of ~ ^ : ? * [ \
     *
     * The '-' rules exist so a URL or ref can never be mistaken for a
     * command-line option when it's later handed to git.
     */
    struct Dependency {
        std::string name;
        std::string url;
        std::string ref;

        /* The first rule this dependency breaks, or std::nullopt if valid. */
        [[nodiscard]] std::optional<std::string> validationError() const;

        /* Throws std::invalid_argument describing the first broken rule. */
        void validate() const;

        /*
         * True if both come from the same place: identical ref, and the
         * same URL after normalizeUrl(). The name is deliberately ignored.
         */
        [[nodiscard]] bool sameSource(const Dependency& other) const;

        /*
         * Canonical form of a Git URL, used by sameSource(): trims
         * whitespace, drops trailing '/' and a trailing ".git", and
         * lowercases the scheme and host of "scheme://host/..." URLs. The
         * path and any user info are case-sensitive and left alone.
         */
        [[nodiscard]] static std::string normalizeUrl(std::string url);
    };

    /*
     * The manifest's optional "build" table. "sn build" reads all of it;
     * "sn run" uses "libraries" and "assets" too, so a program behaves the
     * same before and after it's built.
     *
     *     build = {
     *         output     = "dist/app",         -- ".exe" is added on Windows
     *         libraries  = { "fs", "term" },  -- built-ins this package uses (nil = all)
     *         assets     = { "assets" },       -- files/folders for @sonata/pocket
     *         optimize   = 1,                  -- Luau optimization level, 0..2
     *         debug      = 1,                  -- Luau debug level, 0..2
     *         compress   = true,               -- compress the packed program
     *         standalone = false,              -- no shared-library dependencies at all
     *     },
     *
     * Fields left out are std::nullopt (or empty), so callers can tell "not
     * given" from a value. A dependency's manifest only contributes
     * "libraries" and "assets"; the rest is the root project's call.
     *
     * Library names are not checked here (see selection.hpp), so a manifest
     * naming a library this Sonata doesn't know still opens.
     */
    struct Build {
        std::optional<std::string> output;
        std::optional<std::vector<std::string>> libraries;
        std::vector<std::string> assets;
        std::optional<int> optimize;
        std::optional<int> debug;
        std::optional<bool> compress;
        std::optional<bool> standalone;
    };

    /*
     * Parsed contents of .sonata/project.luau.
     *
     * "entrypoint" is stored exactly as written in the manifest (e.g.
     * "main.luau" or "src/init.luau"); Project::entrypoint() gives you the
     * resolved, absolute path.
     *
     * "dependencies" holds unique names; see Dependency.
     */
    struct Manifest {
        std::string name;
        std::string version;
        std::string description;
        std::vector<std::string> authors;
        std::string entrypoint;
        std::vector<Dependency> dependencies;
        Build build;

        /*
         * True if project.luau starts with a "--!strict" line. Anything that
         * would only be a warning about this package's build settings (such
         * as a missing build.libraries) becomes an error instead. In the root
         * project it applies to every dependency as well.
         */
        bool strict = false;
    };

    /**
     * Open a Sonata project from a directory.
     *
     * The directory must contain:
     *   .sonata/project.luau
     *   <entrypoint>            (as named by project.luau; "main.luau" if
     *                            the manifest doesn't specify one)
     *
     * Throws std::runtime_error if the project is invalid, if
     * project.luau can't be parsed, or if it contains an invalid or
     * duplicate dependency.
     */
    static Project open(const std::filesystem::path& root);

    /**
     * Find and open a project starting from a path.
     *
     * If the path is a file, its parent directory is used.
     * The directory and its parents are searched for .sonata/project.luau.
     */
    static Project find(const std::filesystem::path& start);

    const std::filesystem::path& root() const noexcept;
    const std::filesystem::path& entrypoint() const noexcept;
    const std::filesystem::path& sonataDir() const noexcept;
    const std::filesystem::path& projectFile() const noexcept;
    const std::filesystem::path& dependenciesDir() const noexcept;

    const Manifest& manifest() const noexcept;

    /**
     * Mutable access to the in-memory manifest, mainly so callers can edit
     * manifest().dependencies. Nothing is written to disk until
     * saveManifest() is called.
     *
     * Note that changing manifest().entrypoint here does not update
     * entrypoint(); that is only resolved when the project is opened.
     */
    Manifest& manifest() noexcept;

    /**
     * Write the manifest back to .sonata/project.luau.
     *
     * Throws std::invalid_argument if a dependency is invalid or two share
     * a name, and std::runtime_error if the file can't be read or written.
     * The file is replaced atomically (temp file + rename), so a failed
     * save leaves the old manifest untouched.
     *
     * Top-level keys this class doesn't manage (including "build") are
     * preserved as they were, and a "--!strict" line is written back if
     * manifest().strict is set. Other comments and formatting are not kept:
     * the file is regenerated.
     */
    void saveManifest() const;

    /** The dependency called "name", or nullptr if there isn't one. */
    const Dependency* findDependency(const std::string& name) const noexcept;

    /**
     * Add a dependency, or replace the existing one with the same name,
     * then save the manifest.
     *
     * Returns true if an existing entry was replaced, false if the
     * dependency was appended. Throws std::invalid_argument if the
     * dependency is invalid. If saving fails, the in-memory manifest is
     * rolled back and the exception is rethrown.
     */
    bool addDependency(Dependency dependency);

    /**
     * Remove the dependency called "name" and save the manifest.
     *
     * Returns false (and does nothing) if there is no such dependency. If
     * saving fails, the in-memory manifest is rolled back and the exception
     * is rethrown.
     */
    bool removeDependency(const std::string& name);

    /**
     * Return all .luau source files in the project root.
     *
     * Files inside the .sonata directory are excluded.
     */
    std::vector<std::filesystem::path> sourceFiles() const;

    bool hasDependenciesDirectory() const noexcept;

private:
    explicit Project(std::filesystem::path root);

    void validate();
    void loadManifest();

    std::filesystem::path root_;
    std::filesystem::path sonataDir_;
    std::filesystem::path projectFile_;
    std::filesystem::path dependenciesDir_;
    std::filesystem::path entrypoint_;

    Manifest manifest_;
};

} // namespace sonata