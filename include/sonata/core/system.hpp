#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/*
 * Small OS helpers shared by the package manager, "sn build" and the runtime.
 */
namespace sonata::os {

using EnvList = std::vector<std::pair<std::string, std::string>>;

struct ProcessResult {
    bool started = false; // false if the executable couldn't be launched at all
    int exitCode = -1;
    std::string output;   // stdout and stderr, interleaved (the last 64 KiB)
};

/*
 * Runs args[0] (looked up on PATH) with the given arguments and waits for it.
 * No shell is involved, so nothing in an argument can be read as shell syntax.
 * stdin is the null device. "env" adds to / overrides the current environment.
 *
 * Output is captured into ProcessResult::output, unless "inheritOutput" is
 * true: then the child writes straight to this process's stdout/stderr and
 * output stays empty.
 */
ProcessResult runProcess(const std::vector<std::string>& args, const EnvList& env = {},
                         bool inheritOutput = false);

/* The running executable's own path, or nullopt if the OS won't say. */
std::optional<std::filesystem::path> executablePath();

/* $HOME, or %USERPROFILE% on Windows. nullopt if neither is set. */
std::optional<std::filesystem::path> homeDirectory();

} // namespace sonata::os
