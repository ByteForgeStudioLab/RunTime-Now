#pragma once

// .env files: KEY=value lines loaded into the environment before any code runs,
// so process.env and child processes see them.

#include <string>
#include <string_view>
#include <vector>

namespace rtn::dotenv {

struct Entry {
    std::string key;
    std::string value;
};

// Parses .env text: `KEY=value`, `export KEY=value`, '#' comments, 'single' (literal),
// "double" (escapes, multi-line) and `backtick` quotes, ${VAR} / $VAR expansion
// (unquoted and double-quoted values, plus ${VAR:-default}). Expansion looks at
// `lookup` (the environment) first, then at the entries before it.
std::vector<Entry> parse(std::string_view text, const char* (*lookup)(const char*));

// `explicit_files` (from --env-file) replace the automatic ones; a missing one is an
// error (message in `error`). Otherwise loads whichever of these exist in the current
// directory: .env.local (not when NODE_ENV=test), .env.$NODE_ENV, .env.
// Variables already in the environment always win, then earlier files over later
// ones (for --env-file: later over earlier, like Node).
bool load(const std::vector<std::string>& explicit_files, bool auto_load, std::string& error);

}  // namespace rtn::dotenv
