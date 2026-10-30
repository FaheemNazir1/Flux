#pragma once

#include "flux/app_paths.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace flux {

// Runs one Flux command line. `args` are the arguments after the program name (UTF-8), `env`
// supplies environment variables (FLUX_DB, LOCALAPPDATA, ...), normal output goes to `out`,
// and errors, warnings and progress to `err`. Returns the process exit code:
//   0 success, 1 runtime error (bad path, unavailable drive, database problem, nothing indexed),
//   2 usage error (unknown command or option, missing argument).
// Taking its streams and environment as parameters is what lets tests drive every command
// against a temporary database without touching the user's real one.
int runCli(const std::vector<std::string>& args, const EnvLookup& env, std::ostream& out, std::ostream& err);

}  // namespace flux
