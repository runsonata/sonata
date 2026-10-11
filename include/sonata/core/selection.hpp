#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <sonata/core/project.hpp>

namespace sonata {

/*
 * LIBRARY SELECTION
 *
 * Which built-in libraries a program may use, worked out from the
 * build.libraries list of the root project and of every installed dependency
 * (each dependency is a project of its own, with its own manifest):
 *
 *   - The result is the union of all those lists.
 *   - A manifest without build.libraries means "every library". For the root
 *     that's the normal default; for a dependency it's a warning (only shown
 *     when the root does declare a list, since otherwise nothing changes).
 *   - A manifest starting with "--!strict" turns its own missing list into an
 *     error. A strict root project does the same for every dependency.
 *   - A name Sonata doesn't know is always an error.
 *
 * "sn build" compiles exactly these libraries into the program, and "sn run"
 * refuses the others, so a script can't depend on something the built
 * program won't have.
 */

class BuildError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/* A package from the root project's .sonata/deps, opened. */
struct InstalledProject {
    std::string name; // folder name, i.e. what "@name" refers to
    Project project;
};

/*
 * Opens every package in root's .sonata/deps, sorted by name. Throws
 * BuildError if one of them isn't a valid project.
 */
std::vector<InstalledProject> openDependencies(const Project& root);

struct LibraryOptions {
    // Replaces the root manifest's build.libraries (sn build --libs).
    std::optional<std::vector<std::string>> rootLibraries;

    // Every library, whatever the manifests say (sn build --all-libs).
    bool all = false;

    // Treat everything as if the root manifest were --!strict.
    bool strict = false;
};

struct LibrarySelection {
    std::vector<std::string> libraries; // sorted, no duplicates
    bool all = false;                   // libraries == every known library
    std::vector<std::string> warnings;  // for the user; never fatal

    // What to tell someone who requires a library that isn't selected.
    std::string hint;
};

/* Throws BuildError (listing every problem at once) on any error. */
LibrarySelection selectLibraries(const Project& root,
                                 const std::vector<InstalledProject>& dependencies,
                                 const LibraryOptions& options = {});

} // namespace sonata
