#pragma once

#include <string>

#include "quickjs.h"

namespace rtn {

// Hooks our resolver + loader into the engine so `import` works.
void install_module_loader(JSRuntime* rt);

// Reads a source file and turns it into JavaScript the engine can run
// (e.g. wraps .json files). On failure throws a JS error and returns false.
bool load_source(JSContext* ctx, const std::string& path, std::string& out);

// Fills import.meta (url, filename, dirname, main) for a compiled module.
void set_import_meta(JSContext* ctx, JSValueConst module_fn, const std::string& path, bool is_main);

}  // namespace rtn
