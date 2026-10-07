#pragma once

#include <string>
#include <vector>

namespace rtn {

// `rtn --watch ...`: runs `self args...` and restarts it when a source file in the
// current directory tree changes. Returns when interrupted (128 + signal).
int run_watch(const std::string& self, const std::vector<std::string>& args);

}  // namespace rtn
