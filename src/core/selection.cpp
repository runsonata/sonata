#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>

#include <sonata/core/library.hpp>
#include <sonata/core/selection.hpp>

namespace fs = std::filesystem;

namespace sonata {

namespace {

// Same rule PackageManager uses for folder names in .sonata/deps (which also
// skips ".staging").
bool isPackageName(const std::string& name) {
    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    });
}

std::size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);

    for (std::size_t j = 0; j <= b.size(); ++j) {
        prev[j] = j;
    }

    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }

    return prev[b.size()];
}

std::string unknownLibrary(const std::string& where, const std::string& name) {
    std::string message = where + ": unknown built-in library '" + name + "'";

    const std::string* best = nullptr;
    std::size_t bestDistance = 3;

    for (const std::string& known : lib::knownLibraryNames()) {
        const std::size_t d = editDistance(name, known);
        if (d < bestDistance) {
            bestDistance = d;
            best = &known;
        }
    }

    if (best != nullptr) {
        message += " (did you mean '" + *best + "'?)";
    }

    return message;
}

std::string manifestOf(const Project& project) {
    return project.projectFile().generic_string();
}

} // namespace

std::vector<InstalledProject> openDependencies(const Project& root) {
    std::vector<InstalledProject> result;

    std::error_code ec;
    if (!fs::is_directory(root.dependenciesDir(), ec)) {
        return result;
    }

    for (fs::directory_iterator it(root.dependenciesDir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }

        const std::string name = it->path().filename().string();
        if (!isPackageName(name)) {
            continue;
        }

        try {
            result.push_back(InstalledProject{name, Project::open(it->path())});
        } catch (const std::exception& e) {
            throw BuildError("dependency '" + name + "' is not a valid project: " + e.what());
        }
    }

    std::sort(result.begin(), result.end(),
              [](const InstalledProject& a, const InstalledProject& b) { return a.name < b.name; });

    return result;
}

LibrarySelection selectLibraries(const Project& root,
                                 const std::vector<InstalledProject>& dependencies,
                                 const LibraryOptions& options) {
    const std::vector<std::string>& known = lib::knownLibraryNames();
    const bool rootStrict = options.strict || root.manifest().strict;

    LibrarySelection selection;
    std::vector<std::string> errors;
    std::set<std::string> chosen;
    bool all = options.all;

    const auto addList = [&](const std::vector<std::string>& names, const std::string& where) {
        for (const std::string& name : names) {
            if (std::binary_search(known.begin(), known.end(), name)) {
                chosen.insert(name);
            } else {
                errors.push_back(unknownLibrary(where, name));
            }
        }
    };

    // The root project.
    const std::optional<std::vector<std::string>>& rootList =
        options.rootLibraries ? options.rootLibraries : root.manifest().build.libraries;

    if (rootList) {
        addList(*rootList, options.rootLibraries ? "--libs" : manifestOf(root));
    } else if (!options.all) {
        if (rootStrict) {
            errors.push_back(manifestOf(root) + ": the project is --!strict, so it must list the "
                             "built-in libraries it uses in build.libraries");
        }
        all = true;
    }

    const bool rootDeclares = rootList.has_value() && !options.all;

    // Every dependency.
    for (const InstalledProject& dependency : dependencies) {
        const Project::Manifest& manifest = dependency.project.manifest();

        if (manifest.build.libraries) {
            addList(*manifest.build.libraries, manifestOf(dependency.project));
            continue;
        }

        if (manifest.strict) {
            errors.push_back("dependency '" + dependency.name + "' is --!strict but doesn't list the "
                             "built-in libraries it uses in build.libraries");
        } else if (rootStrict) {
            errors.push_back("dependency '" + dependency.name + "' doesn't list the built-in libraries "
                             "it uses in build.libraries (required because the project is --!strict)");
        } else if (rootDeclares) {
            selection.warnings.push_back("dependency '" + dependency.name + "' doesn't list the built-in "
                                         "libraries it uses in build.libraries, so every library is included");
        }

        all = true;
    }

    if (!errors.empty()) {
        std::string message = errors.size() == 1 ? errors.front() : "the library selection has problems:";
        if (errors.size() > 1) {
            for (const std::string& error : errors) {
                message += "\n  - " + error;
            }
        }
        throw BuildError(message);
    }

    if (all) {
        selection.libraries = known;
        selection.all = true;
    } else {
        selection.libraries.assign(chosen.begin(), chosen.end());
        selection.all = selection.libraries.size() == known.size();
    }

    selection.hint = "add it to build.libraries in " + manifestOf(root);
    return selection;
}

} // namespace sonata
