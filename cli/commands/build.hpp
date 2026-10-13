#pragma once

#include "command.hpp"

namespace sonata::commands {

const cli::Command& build_command();

/*
 * sn build [path] [options]
 *
 * Compiles a Sonata project (and its dependencies) into one standalone
 * executable that runs without Sonata installed.
 *
 * Returns 0 on success, 1 on any failure (bad arguments, no project, compile
 * errors, a library that isn't selected, or no usable runtime).
 */
int build(int argc, char** argv);

}
