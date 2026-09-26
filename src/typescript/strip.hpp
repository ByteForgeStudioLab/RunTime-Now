#pragma once

#include <string>
#include <string_view>

namespace rtn::ts {

struct StripResult {
    bool ok = false;
    std::string code;   // JavaScript output (when ok)
    std::string error;  // "message" (when !ok)
    int line = 0;       // 1-based position of the error
    int column = 0;
};

// Turns TypeScript into JavaScript by erasing type syntax.
//
// Types are replaced with spaces (newlines are kept), so every line and
// column in the output matches the original .ts file — stack traces point
// at the right place without source maps.
//
// A few TS features produce real code and are transformed instead:
//   - enum / const enum
//   - constructor parameter properties: constructor(private x: number)
// Imports that are only used as types are removed (like tsc does).
StripResult strip_types(std::string_view source);

}  // namespace rtn::ts
