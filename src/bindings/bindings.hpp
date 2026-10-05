#pragma once

#include <string>

#include "quickjs.h"

namespace rtn {

// Globals
void install_console(JSContext* ctx);
void install_timers(JSContext* ctx);
void install_process(JSContext* ctx);
// Runs the embedded JS (src/js/*.js): URL, Headers, Request, Response, rtn.serve...
void install_builtins(JSContext* ctx);

// Pretty-prints a value the way console.log does (used by the REPL too).
std::string inspect(JSContext* ctx, JSValueConst v, bool colors);

// Built-in modules (src/js/modules.js and friends): "fs", "path", "events"...
// Returns a new reference to the exports object (undefined if there's none).
JSValue builtin_module_exports(JSContext* ctx, const std::string& name);
// Calls internal[fn](...args) — JS helpers of the module system (src/js/cjs.js).
JSValue call_internal(JSContext* ctx, const char* fn, int argc, JSValueConst* argv);

// Low-level functions handed to the embedded JS (never visible to user code).
void add_encoding_natives(JSContext* ctx, JSValueConst native);
void add_console_natives(JSContext* ctx, JSValueConst native);
void add_timer_natives(JSContext* ctx, JSValueConst native);
void add_http_natives(JSContext* ctx, JSValueConst native);
void add_fetch_natives(JSContext* ctx, JSValueConst native);
void add_crypto_natives(JSContext* ctx, JSValueConst native);
void add_fs_natives(JSContext* ctx, JSValueConst native);
void add_module_natives(JSContext* ctx, JSValueConst native);
void add_os_natives(JSContext* ctx, JSValueConst native);

}  // namespace rtn
