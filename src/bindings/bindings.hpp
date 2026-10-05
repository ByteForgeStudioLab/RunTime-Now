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

// Native modules (import ... from "rtn:fs")
JSModuleDef* create_fs_module(JSContext* ctx, const char* name);
// Built-in modules written in JS (src/js/modules.js): "path", "fs/promises".
// Returns a new reference to the exports object (undefined if there's none).
JSValue builtin_module_exports(JSContext* ctx, const std::string& name);

// Low-level functions handed to the embedded JS (never visible to user code).
void add_encoding_natives(JSContext* ctx, JSValueConst native);
void add_timer_natives(JSContext* ctx, JSValueConst native);
void add_http_natives(JSContext* ctx, JSValueConst native);
void add_fetch_natives(JSContext* ctx, JSValueConst native);
void add_crypto_natives(JSContext* ctx, JSValueConst native);
void add_fs_natives(JSContext* ctx, JSValueConst native);

}  // namespace rtn
