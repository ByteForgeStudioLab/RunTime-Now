#pragma once

#include <string>

namespace rtn {

// `rtn upgrade [options]` / `rtn update [options]`. argv holds only the options.
// self_path is the absolute path of the running binary (the file that gets replaced).
int run_upgrade(int argc, char** argv, const std::string& self_path);

}  // namespace rtn
