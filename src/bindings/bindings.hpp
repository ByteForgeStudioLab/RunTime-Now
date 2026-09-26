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

// Low-level functions handed to the embedded JS (never visible to user code).
void add_encoding_natives(JSContext* ctx, JSValueConst native);
void add_http_natives(JSContext* ctx, JSValueConst native);

}  // namespace rtn
