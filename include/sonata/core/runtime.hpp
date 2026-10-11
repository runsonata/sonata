#pragma once

#include <string>
#include <vector>

#include <sonata/core/module.hpp>
#include <sonata/core/pocket.hpp>

namespace sonata::runtime {

/*
 * What a running program knows about itself. Becomes the "sonata" global:
 *
 *     sonata.version          Sonata's version, "0.1.0"
 *     sonata.platform         "linux-x64"
 *     sonata.entry            module path of the entry script
 *     sonata.bundled          true inside a program made by "sn build"
 *     sonata.executable       path of the program's executable (bundled only)
 *     sonata.project.name     from the manifest
 *     sonata.project.version  from the manifest
 *     sonata.project.root     the project folder under "sn run"; the folder
 *                             holding the executable in a built program
 */
struct ProgramInfo {
    std::string name;
    std::string version;
    std::string root;
    std::string entry;
    bool bundled = false;
    std::string executable;
};

/*
 * Runs a program. "sn run" and built executables both come through here, so
 * they behave the same:
 *
 *   1. Creates the VM with the "sonata" and "args" globals ("args" is the
 *      array of strings given to the program).
 *   2. Installs require() from "loader" (which must already have its aliases
 *      and library restrictions) and makes "pocket" available to
 *      @sonata/pocket (may be nullptr).
 *   3. Sandboxes the globals and runs the entry.
 *
 * "loader" and "pocket" must outlive the call. Throws luau::ModuleError
 * (with a stack trace) if the script fails.
 */
void runProgram(luau::ModuleLoader& loader, const ProgramInfo& info,
                const std::vector<std::string>& args, const Pocket* pocket);

} // namespace sonata::runtime
