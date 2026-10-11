// snrt: the Sonata runtime.
//
// "sn build" copies this executable and appends a bundle (see
// sonata/core/bundle.hpp) holding the compiled program. At startup the runtime
// reads that bundle back out of its own file and runs it exactly the way
// "sn run" runs a project (see sonata/core/runtime.hpp).
//
// On its own, without a bundle, it does nothing.

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sonata/core/bundle.hpp>
#include <sonata/core/info.hpp>
#include <sonata/core/module.hpp>
#include <sonata/core/pocket.hpp>
#include <sonata/core/runtime.hpp>
#include <sonata/core/system.hpp>

namespace fs = std::filesystem;

namespace {

int fail(const std::string &message) {
    std::cerr << "error: " << message << '\n';
    return 1;
}

int runBundle(sonata::bundle::Bundle bundle, const fs::path &executable,
              std::vector<std::string> args) {
    using namespace sonata;

    auto source = std::make_unique<luau::MemorySource>();
    for (bundle::Module &module : bundle.modules) {
        source->add(module.path, std::move(module.bytecode));
    }

    // Declared before the loader and the VM, so it outlives both.
    MemoryPocket pocket;

    for (bundle::Package &package : bundle.packages) {
        pocket.addPackage(package.root);
        source->addPackage(package.root, std::move(package.dependencies),
                           std::move(package.entrypoint));
    }

    for (bundle::Asset &asset : bundle.assets) {
        pocket.add(asset.package, asset.path, std::move(asset.data));
    }

    // The loader must outlive the VM (runProgram creates and destroys it).
    luau::ModuleLoader loader(std::move(source));
    for (const auto &[name, directory] : bundle.aliases) {
        loader.addAlias(name, directory);
    }
    loader.restrictLibraries(bundle.libraries, bundle.libraryHint);

    runtime::ProgramInfo info;
    info.name = bundle.name;
    info.version = bundle.version;
    info.root = executable.parent_path().generic_string();
    info.entry = bundle.entry;
    info.bundled = true;
    info.executable = executable.generic_string();

    runtime::runProgram(loader, info, args, &pocket);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    using namespace sonata;

    try {
        const std::optional<fs::path> executable = os::executablePath();
        if (!executable) {
            return fail("could not find this program's own executable file");
        }

        std::optional<bundle::Bundle> program = bundle::read(*executable);
        if (!program) {
            std::cerr << info::name << " runtime " << info::version_string()
                      << "\nThis is the runtime that '" << info::binary
                      << " build' turns into standalone programs; it has no "
                         "program of its own.\n";
            return 1;
        }

        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }

        return runBundle(std::move(*program), *executable, std::move(args));
    } catch (const luau::ModuleError &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        return fail(error.what());
    }
}
