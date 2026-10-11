#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <utility>
#include <sstream>

#include <sonata/core/luau/datafile.hpp>
#include <sonata/core/module.hpp>
#include <sonata/core/package.hpp>
#include <sonata/core/library.hpp>
#include <sonata/core/system.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
extern char **environ;
#endif

#if defined(SONATA_USE_LIBGIT2)
#include <git2.h>
#endif

namespace fs = std::filesystem;

namespace sonata {

// Helpers
namespace {

using os::EnvList;
using os::ProcessResult;
using os::runProcess;

/* Path -> UTF-8 std::string (u8string() returns char8_t-based strings in
 * C++20). */
std::string utf8(const fs::path &p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

std::string lastLine(const std::string &text) {
    const std::string t = trim(text);
    const auto pos = t.find_last_of('\n');
    return trim(pos == std::string::npos ? std::string_view(t)
                                         : std::string_view(t).substr(pos + 1));
}

bool isFullCommitHash(std::string_view s) {
    if (s.size() != 40 && s.size() != 64)
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

bool hasControlChar(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7f;
    });
}

bool isValidName(std::string_view name) {
    if (name.empty() || name.size() > 64)
        return false;
    const auto alpha = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    if (!alpha(name.front()))
        return false;
    return std::all_of(name.begin(), name.end(), [&](char c) {
        return alpha(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
}

/* Good enough to notice "same repository" when comparing two declarations. */
std::string normalizeUrl(std::string url) {
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0)
        url.resize(url.size() - 4);
    return url;
}

bool sameSource(const Project::Dependency &a, const Project::Dependency &b) {
    return normalizeUrl(a.url) == normalizeUrl(b.url) && a.ref == b.ref;
}

std::string describe(const Project::Dependency &d) {
    return "'" + d.name + "' (" + d.url + (d.ref.empty() ? "" : "#" + d.ref) +
           ")";
}

std::string randomSuffix() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr char digits[] = "0123456789abcdef";
    std::uint64_t v = rng();
    std::string out(12, '0');
    for (char &c : out) {
        c = digits[v & 0xf];
        v >>= 4;
    }
    return out;
}

template <class F> class ScopeExit {
  public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() {
        try {
            f_();
        } catch (...) {
        }
    }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

  private:
    F f_;
};

/*
 * Deletes a directory tree. git marks pack files read-only, which makes
 * remove_all fail on Windows, so on failure everything is made writable and
 * the removal is retried.
 */
void removeTree(const fs::path &p) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec)))
        return;

    fs::remove_all(p, ec);
    if (!ec)
        return;

    std::error_code ignored;
    for (fs::recursive_directory_iterator
             it(p, fs::directory_options::skip_permission_denied, ignored),
         end;
         !ignored && it != end; it.increment(ignored)) {
        std::error_code permEc;
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add,
                        permEc);
    }
    std::error_code permEc;
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add, permEc);

    fs::remove_all(p, ec);
    if (ec)
        throw PackageError("could not remove '" + utf8(p) +
                           "': " + ec.message());
}

void removeTreeQuiet(const fs::path &p) noexcept {
    try {
        removeTree(p);
    } catch (...) {
    }
}

} // namespace

// Git backend 1: the git command line
namespace {

class CliGitBackend final : public GitBackend {
  public:
    std::string_view name() const noexcept override { return "git"; }

    bool available() const override {
        if (!available_) {
            const ProcessResult r = runProcess({"git", "--version"}, {});
            available_ = r.started && r.exitCode == 0;
        }
        return *available_;
    }

    std::string clone(const std::string &url, const fs::path &destination,
                      const std::string &ref) const override {
        const std::string target = utf8(destination);

        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);

        bool done = false;

        if (ref.empty()) {
            expectOk(
                git({"clone", "--quiet", "--depth", "1", "--", url, target}),
                "git clone");
            done = true;
        } else if (!isFullCommitHash(ref)) {
            // Cheap path for branches and tags. Fails for commit hashes (and
            // anything else --branch can't name), which fall through below.
            const ProcessResult r = git({"clone", "--quiet", "--depth", "1",
                                         "--branch", ref, "--", url, target});
            done = r.exitCode == 0;
            if (!done)
                removeTreeQuiet(destination);
        }

        if (!done) {
            expectOk(git({"clone", "--quiet", "--", url, target}), "git clone");

            bool checkedOut = false;
            for (const std::string &candidate : {ref, "origin/" + ref}) {
                const ProcessResult r =
                    git({"-C", target, "-c", "advice.detachedHead=false",
                         "checkout", "--quiet", "--detach", candidate, "--"});
                if (r.exitCode == 0) {
                    checkedOut = true;
                    break;
                }
            }
            if (!checkedOut)
                throw PackageError(
                    "'" + ref + "' is not a branch, tag or commit of " + url);
        }

        const ProcessResult head = git({"-C", target, "rev-parse", "HEAD"});
        expectOk(head, "git rev-parse");
        const std::string commit = lastLine(head.output);
        if (commit.size() < 40 ||
            !std::all_of(commit.begin(), commit.end(), [](char c) {
                return std::isxdigit(static_cast<unsigned char>(c)) != 0;
            })) {
            throw PackageError("git rev-parse returned something unexpected: " +
                               commit);
        }
        return commit;
    }

  private:
    static ProcessResult git(std::vector<std::string> args) {
        // Never prompt on the terminal, and only allow transports we expect.
        static const EnvList env = {
            {"GIT_TERMINAL_PROMPT", "0"},
            {"GIT_ALLOW_PROTOCOL", "file:git:http:https:ssh"},
        };
        args.insert(args.begin(), "git");
        const ProcessResult r = runProcess(args, env);
        if (!r.started)
            throw PackageError("could not run the git executable");
        return r;
    }

    static void expectOk(const ProcessResult &r, const char *what) {
        if (r.exitCode == 0)
            return;
        std::string message = std::string(what) + " failed (exit code " +
                              std::to_string(r.exitCode) + ")";
        const std::string detail = trim(r.output);
        if (!detail.empty())
            message += ": " + detail;
        throw PackageError(message);
    }

    mutable std::optional<bool> available_;
};

} // namespace

std::unique_ptr<GitBackend> makeCliGitBackend() {
    return std::make_unique<CliGitBackend>();
}

// Git backend 2: libgit2 (optional)
#if defined(SONATA_USE_LIBGIT2)

namespace {

struct RepoDeleter {
    void operator()(git_repository *r) const { git_repository_free(r); }
};
struct ObjectDeleter {
    void operator()(git_object *o) const { git_object_free(o); }
};
using RepoPtr = std::unique_ptr<git_repository, RepoDeleter>;
using ObjectPtr = std::unique_ptr<git_object, ObjectDeleter>;

std::string gitError() {
    const git_error *e = git_error_last();
    return (e && e->message) ? e->message : "unknown libgit2 error";
}

ObjectPtr resolveCommit(git_repository *repo, const std::string &ref) {
    const std::string candidates[] = {ref, "origin/" + ref};
    for (const std::string &spec : candidates) {
        git_object *raw = nullptr;
        if (git_revparse_single(&raw, repo, spec.c_str()) != 0)
            continue;
        ObjectPtr object(raw);

        git_object *commit = nullptr;
        if (git_object_peel(&commit, object.get(), GIT_OBJECT_COMMIT) == 0)
            return ObjectPtr(commit);
    }
    throw PackageError("'" + ref +
                       "' is not a branch, tag or commit of the repository");
}

class LibGit2Backend final : public GitBackend {
  public:
    LibGit2Backend() : ready_(git_libgit2_init() > 0) {}
    ~LibGit2Backend() override {
        if (ready_)
            git_libgit2_shutdown();
    }

    std::string_view name() const noexcept override { return "libgit2"; }
    bool available() const override { return ready_; }

    std::string clone(const std::string &url, const fs::path &destination,
                      const std::string &ref) const override {
        const std::string target = utf8(destination);

        git_clone_options options;
        if (git_clone_options_init(&options, GIT_CLONE_OPTIONS_VERSION) != 0) {
            throw PackageError("libgit2: could not initialise clone options");
        }

        git_repository *raw = nullptr;
        if (git_clone(&raw, url.c_str(), target.c_str(), &options) != 0) {
            throw PackageError("libgit2 clone failed: " + gitError());
        }
        RepoPtr repo(raw);

        git_oid oid;
        if (ref.empty()) {
            if (git_reference_name_to_id(&oid, repo.get(), "HEAD") != 0) {
                throw PackageError("libgit2: could not resolve HEAD: " +
                                   gitError());
            }
        } else {
            const ObjectPtr commit = resolveCommit(repo.get(), ref);

            git_checkout_options checkout;
            if (git_checkout_options_init(&checkout,
                                          GIT_CHECKOUT_OPTIONS_VERSION) != 0) {
                throw PackageError(
                    "libgit2: could not initialise checkout options");
            }
            checkout.checkout_strategy = GIT_CHECKOUT_FORCE;

            if (git_checkout_tree(repo.get(), commit.get(), &checkout) != 0) {
                throw PackageError("libgit2 checkout failed: " + gitError());
            }
            oid = *git_object_id(commit.get());
            if (git_repository_set_head_detached(repo.get(), &oid) != 0) {
                throw PackageError("libgit2: could not move HEAD: " +
                                   gitError());
            }
        }

        char hex[65] = {};
        git_oid_tostr(hex, sizeof(hex), &oid);
        return hex;
    }

  private:
    bool ready_;
};

} // namespace

std::unique_ptr<GitBackend> makeLibGit2Backend() {
    return std::make_unique<LibGit2Backend>();
}

#else

std::unique_ptr<GitBackend> makeLibGit2Backend() { return nullptr; }

#endif

std::vector<std::unique_ptr<GitBackend>> defaultGitBackends() {
    std::vector<std::unique_ptr<GitBackend>> backends;
    backends.push_back(makeCliGitBackend());
    if (auto fallback = makeLibGit2Backend())
        backends.push_back(std::move(fallback));
    return backends;
}

// PackageManager
PackageManager::PackageManager(Project &project)
    : PackageManager(project, defaultGitBackends()) {}

PackageManager::PackageManager(
    Project &project, std::vector<std::unique_ptr<GitBackend>> backends)
    : project_(project), backends_(std::move(backends)) {}

void PackageManager::setProgressCallback(ProgressCallback callback) {
    progress_ = std::move(callback);
}

void PackageManager::say(const std::string &message) const {
    if (progress_)
        progress_(message);
}

fs::path PackageManager::lockFile() const {
    return project_.sonataDir() / "lock.luau";
}

// validation

void PackageManager::validate(const Dependency &d) {
    if (!isValidName(d.name)) {
        throw PackageError(
            "invalid dependency name '" + d.name +
            "': use letters, digits and '_', and don't start with a digit");
    }
    if (d.url.empty()) {
        throw PackageError("dependency '" + d.name + "' has no repository URL");
    }
    if (d.url.front() == '-' || hasControlChar(d.url)) {
        throw PackageError("dependency '" + d.name + "' has an invalid URL");
    }
    if (!d.ref.empty()) {
        const bool bad =
            d.ref.front() == '-' || hasControlChar(d.ref) ||
            d.ref.find_first_of(" ~^:?*[\\") != std::string::npos ||
            d.ref.find("..") != std::string::npos;
        if (bad)
            throw PackageError("dependency '" + d.name +
                               "' has an invalid ref '" + d.ref + "'");
    }
}

// lock file

PackageManager::Lock PackageManager::readLock() const {
    Lock lock;
    const fs::path path = lockFile();

    std::error_code ec;
    if (!fs::exists(path, ec))
        return lock;

    try {
        const luau::DataValue data = luau::DataFile::parseFile(path);
        const luau::DataValue *packages = data.find("packages");
        if (!packages || !packages->isTable())
            return lock;

        for (const auto &entry : packages->asTable()) {
            if (!entry.key ||
                entry.key->kind != luau::DataValue::Key::Kind::String)
                continue;
            if (!entry.value.isTable())
                continue;

            const auto field = [&](const char *key) {
                const luau::DataValue *v = entry.value.find(key);
                return (v && v->isString()) ? v->asString() : std::string();
            };
            lock[entry.key->string] =
                LockEntry{field("url"), field("ref"), field("commit")};
        }
    } catch (const std::exception &e) {
        say(std::string("ignoring unreadable lock file: ") + e.what());
        lock.clear();
    }
    return lock;
}

void PackageManager::writeLock(const Lock &lock) const {
    luau::DataValue packages = luau::DataValue::table();
    for (const auto &[name, entry] : lock) {
        luau::DataValue item = luau::DataValue::table();
        item.set("url", luau::DataValue(entry.url));
        if (!entry.ref.empty())
            item.set("ref", luau::DataValue(entry.ref));
        item.set("commit", luau::DataValue(entry.commit));
        packages.set(name, std::move(item));
    }

    luau::DataValue root = luau::DataValue::table();
    root.set("version", luau::DataValue(1.0));
    root.set("packages", std::move(packages));

    std::error_code ec;
    fs::create_directories(lockFile().parent_path(), ec);
    luau::DataFile::save(root, lockFile());
    {   // write a comment at the top of the lock file to discourage manual
        // editing
        std::string existing_content;
        {
            std::ifstream infile(lockFile());
            if (infile.is_open()) {
                std::stringstream buffer;
                buffer << infile.rdbuf();
                existing_content = buffer.str();
            }
        }

        std::ofstream lockfile(lockFile());
        if (lockfile.is_open()) {
            lockfile
                << "--[[\n"
                   "    This file is automatically generated by Sonata.\n"
                   "    DO NOT EDIT MANUALLY - Changes will be overwritten. \n"
                   "]]\n\n"
                << existing_content;
        } else {
            std::cout << "warning: could not write modification disclosure to "
                         "lock file: "
                      << lockFile() << "\n";
        }
    }
}

// fetching

std::string PackageManager::cloneWithBackends(const Dependency &dep,
                                              const fs::path &destination,
                                              const std::string &ref) const {
    std::string failures;

    for (const auto &backend : backends_) {
        const std::string backendName(backend->name());

        if (!backend->available()) {
            failures += "\n  " + backendName + ": not available";
            continue;
        }

        try {
            removeTree(destination); // a previous backend may have left debris
            return backend->clone(dep.url, destination, ref);
        } catch (const std::exception &e) {
            failures += "\n  " + backendName + ": " + e.what();
            say(backendName + " failed for '" + dep.name + "'");
        }
    }

    if (backends_.empty())
        failures = "\n  no git backend configured";
    throw PackageError("could not fetch " + describe(dep) + failures);
}

InstalledPackage PackageManager::fetch(const Dependency &dep,
                                       const std::string &ref) {
    const fs::path depsDir = project_.dependenciesDir();
    const fs::path stagingRoot = depsDir / ".staging";
    const fs::path stage = stagingRoot / (dep.name + "-" + randomSuffix());
    const fs::path destination = depsDir / dep.name;

    std::error_code ec;
    fs::create_directories(stagingRoot, ec);
    if (ec)
        throw PackageError("could not create '" + utf8(stagingRoot) +
                           "': " + ec.message());

    ScopeExit cleanup([&] {
        removeTreeQuiet(stage);
        std::error_code ignored;
        fs::remove(stagingRoot, ignored); // only succeeds when empty
    });

    say("fetching " + describe(dep) +
        (ref != dep.ref ? " @ " + ref.substr(0, 12) : ""));
    const std::string commit = cloneWithBackends(dep, stage, ref);

    // Don't install anything that isn't a Sonata project.
    try {
        (void)Project::open(stage);
    } catch (const std::exception &e) {
        throw PackageError(describe(dep) +
                           " is not a valid Sonata project (expected "
                           ".sonata/project.luau and its entrypoint): " +
                           e.what());
    }

    // Keep the installed tree free of git metadata.
    removeTree(stage / ".git");

    // Swap into place; if anything goes wrong the previous version is restored.
    fs::path backup;
    if (fs::exists(fs::symlink_status(destination, ec))) {
        backup = stagingRoot / (dep.name + ".old-" + randomSuffix());
        fs::rename(destination, backup, ec);
        if (ec)
            throw PackageError("could not replace '" + utf8(destination) +
                               "': " + ec.message());
    }

    fs::rename(stage, destination, ec);
    if (ec) {
        const std::string reason = ec.message();
        if (!backup.empty()) {
            std::error_code restoreEc;
            fs::rename(backup, destination, restoreEc);
        }
        throw PackageError("could not install '" + utf8(destination) +
                           "': " + reason);
    }
    if (!backup.empty())
        removeTreeQuiet(backup);

    InstalledPackage pkg;
    pkg.dependency = dep;
    pkg.path = destination;
    pkg.commit = commit;
    pkg.fresh = true;
    return pkg;
}

InstalledPackage PackageManager::ensureInstalled(const Dependency &dep,
                                                 Lock &lock,
                                                 const Refresh &refresh) {
    const fs::path destination = project_.dependenciesDir() / dep.name;
    const bool forced = refresh.all || refresh.names.count(dep.name) > 0;

    const auto locked = lock.find(dep.name);
    const bool lockMatches = locked != lock.end() &&
                             locked->second.url == dep.url &&
                             locked->second.ref == dep.ref;

    std::string pinned; // exact commit to restore, if we know one

    if (!forced && lockMatches) {
        std::error_code ec;
        if (fs::is_directory(destination, ec)) {
            try {
                (void)Project::open(destination);
                InstalledPackage pkg;
                pkg.dependency = dep;
                pkg.path = destination;
                pkg.commit = locked->second.commit;
                pkg.fresh = false;
                return pkg;
            } catch (const std::exception &e) {
                say(describe(dep) + " is damaged (" + e.what() +
                    "); reinstalling");
            }
        }
        pinned = locked->second.commit;
    }

    InstalledPackage pkg;
    if (!pinned.empty()) {
        try {
            pkg = fetch(dep, pinned);
        } catch (const PackageError &e) {
            say("locked commit of " + describe(dep) + " unavailable (" +
                e.what() + "); using its ref instead");
            pkg = fetch(dep, dep.ref);
        }
    } else {
        pkg = fetch(dep, dep.ref);
    }

    lock[dep.name] = LockEntry{dep.url, dep.ref, pkg.commit};
    writeLock(lock); // written eagerly so a failure further on doesn't lose
                     // what already worked
    return pkg;
}

// sync

std::vector<InstalledPackage> PackageManager::sync(const Refresh &refresh) {
    std::error_code ec;
    fs::create_directories(project_.dependenciesDir(), ec);
    if (ec)
        throw PackageError("could not create '" +
                           utf8(project_.dependenciesDir()) +
                           "': " + ec.message());

    Lock lock = readLock();

    struct Pending {
        Dependency dep;
        std::string requiredBy; // empty = the root project
    };
    struct Resolved {
        Dependency dep;
        std::string requiredBy;
    };

    // everything the root asked for is resolved before
    // anything a dependency asked for - which is what lets the root manifest
    // win conflicts.
    std::deque<Pending> queue;
    std::set<std::string> rootNames;
    for (const Dependency &d : project_.manifest().dependencies) {
        validate(d);
        rootNames.insert(d.name);
        queue.push_back({d, ""});
    }

    std::map<std::string, Resolved> resolved;
    std::vector<InstalledPackage> order;

    while (!queue.empty()) {
        Pending item = std::move(queue.front());
        queue.pop_front();
        const Dependency &dep = item.dep;

        if (const auto it = resolved.find(dep.name); it != resolved.end()) {
            if (sameSource(it->second.dep, dep))
                continue;

            const std::string by = item.requiredBy.empty()
                                       ? "the project"
                                       : "'" + item.requiredBy + "'";
            if (rootNames.count(dep.name)) {
                say("note: " + by + " wants " + describe(dep) +
                    " but the project's own choice " +
                    describe(it->second.dep) + " is used");
                continue;
            }
            const std::string other = it->second.requiredBy.empty()
                                          ? "the project"
                                          : "'" + it->second.requiredBy + "'";
            throw PackageError(
                "dependency conflict for '" + dep.name + "': " + by +
                " wants " + describe(dep) + " but " + other + " wants " +
                describe(it->second.dep) +
                ". Pin one in the project's own dependencies to resolve it.");
        }

        InstalledPackage pkg = ensureInstalled(dep, lock, refresh);
        resolved.emplace(dep.name, Resolved{dep, item.requiredBy});

        // What does this package need in turn?
        try {
            const Project sub = Project::open(pkg.path);
            for (const Dependency &transitive : sub.manifest().dependencies) {
                validate(transitive);
                queue.push_back({transitive, dep.name});
            }
        } catch (const PackageError &e) {
            throw PackageError("'" + dep.name +
                               "' declares a bad dependency: " + e.what());
        }

        order.push_back(std::move(pkg));
    }

    // Forget lock entries for packages that are no longer part of the graph.
    for (auto it = lock.begin(); it != lock.end();) {
        it = resolved.count(it->first) ? std::next(it) : lock.erase(it);
    }
    writeLock(lock);

    return order;
}

// public API

std::vector<InstalledPackage> PackageManager::install() {
    return sync(Refresh{});
}

std::vector<InstalledPackage> PackageManager::update() {
    Refresh refresh;
    refresh.all = true;
    return sync(refresh);
}

std::vector<InstalledPackage> PackageManager::update(const std::string &name) {
    const auto &deps = project_.manifest().dependencies;
    const bool inManifest =
        std::any_of(deps.begin(), deps.end(),
                    [&](const Dependency &d) { return d.name == name; });
    if (!inManifest && readLock().count(name) == 0) {
        throw PackageError("'" + name +
                           "' is not a dependency of this project");
    }

    Refresh refresh;
    refresh.names.insert(name);
    return sync(refresh);
}

InstalledPackage PackageManager::add(Dependency dependency) {
    validate(dependency);

    auto &deps = project_.manifest().dependencies;
    const std::vector<Dependency> previous = deps;

    const auto it =
        std::find_if(deps.begin(), deps.end(), [&](const Dependency &d) {
            return d.name == dependency.name;
        });
    if (it != deps.end()) {
        *it = dependency;
    } else {
        deps.push_back(dependency);
    }
    project_.saveManifest();

    try {
        const std::vector<InstalledPackage> all = sync(Refresh{});
        for (const InstalledPackage &pkg : all) {
            if (pkg.dependency.name == dependency.name)
                return pkg;
        }
        throw PackageError("internal error: '" + dependency.name +
                           "' was not resolved");
    } catch (...) {
        deps = previous;
        try {
            project_.saveManifest();
        } catch (...) {
        }
        throw;
    }
}

std::vector<std::string> PackageManager::remove(const std::string &name) {
    auto &deps = project_.manifest().dependencies;
    const auto it =
        std::find_if(deps.begin(), deps.end(),
                     [&](const Dependency &d) { return d.name == name; });
    if (it == deps.end())
        throw PackageError("'" + name +
                           "' is not a dependency of this project");

    const std::vector<Dependency> previous = deps;
    deps.erase(it);
    try {
        project_.saveManifest();
    } catch (...) {
        deps = previous;
        throw;
    }

    return prune();
}

std::vector<std::string> PackageManager::prune() {
    // Everything reachable from the root manifest through installed packages.
    std::set<std::string> reachable;
    std::deque<std::string> queue;
    for (const Dependency &d : project_.manifest().dependencies)
        queue.push_back(d.name);

    while (!queue.empty()) {
        const std::string name = std::move(queue.front());
        queue.pop_front();
        if (!reachable.insert(name).second)
            continue;

        try {
            const Project sub =
                Project::open(project_.dependenciesDir() / name);
            for (const Dependency &d : sub.manifest().dependencies)
                queue.push_back(d.name);
        } catch (const std::exception &) {
            // Not installed (or damaged): nothing to follow.
        }
    }

    std::vector<std::string> removed;
    std::error_code ec;
    const fs::path depsDir = project_.dependenciesDir();
    if (fs::is_directory(depsDir, ec)) {
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(depsDir, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_directory(ec))
                entries.push_back(it->path());
        }

        for (const fs::path &entry : entries) {
            const std::string name = utf8(entry.filename());
            if (!name.empty() && name.front() == '.')
                continue; // .staging etc.
            if (reachable.count(name))
                continue;
            removeTree(entry);
            removed.push_back(name);
        }
    }
    std::sort(removed.begin(), removed.end());

    if (!removed.empty()) {
        Lock lock = readLock();
        for (const std::string &name : removed)
            lock.erase(name);
        writeLock(lock);
    }
    return removed;
}

std::vector<InstalledPackage> PackageManager::installed() const {
    std::vector<InstalledPackage> result;

    std::error_code ec;
    const fs::path depsDir = project_.dependenciesDir();
    if (!fs::is_directory(depsDir, ec))
        return result;

    const Lock lock = readLock();

    for (fs::directory_iterator it(depsDir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec))
            continue;

        const std::string name = utf8(it->path().filename());
        if (!isValidName(name))
            continue; // also skips ".staging"

        InstalledPackage pkg;
        pkg.dependency.name = name;
        pkg.path = it->path();
        if (const auto l = lock.find(name); l != lock.end()) {
            pkg.dependency.url = l->second.url;
            pkg.dependency.ref = l->second.ref;
            pkg.commit = l->second.commit;
        }
        result.push_back(std::move(pkg));
    }

    std::sort(result.begin(), result.end(),
              [](const InstalledPackage &a, const InstalledPackage &b) {
                  return a.dependency.name < b.dependency.name;
              });
    return result;
}

bool PackageManager::isInstalled(const std::string &name) const {
    if (!isValidName(name))
        return false;
    std::error_code ec;
    return fs::is_directory(project_.dependenciesDir() / name, ec);
}

void PackageManager::registerAliases(luau::ModuleLoader &loader) const {
    for (const InstalledPackage &pkg : installed()) {
        std::string directory = utf8(fs::absolute(pkg.path).lexically_normal());
        std::replace(directory.begin(), directory.end(), '\\', '/');
        loader.addAlias(pkg.dependency.name, directory);
    }
}

} // namespace sonata