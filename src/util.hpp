#pragma once

#include <optional>
#include <string>

#include "quickjs.h"

namespace rtn {

// Reads a whole file into a string. Returns nullopt if it can't be opened.
std::optional<std::string> read_file(const std::string& path);

// Converts a JS value to std::string (like String(v)).
std::string to_string(JSContext* ctx, JSValueConst v);

bool ends_with(const std::string& s, const std::string& suffix);

// Prints an exception (with stack trace) to stderr.
void print_exception(JSContext* ctx, JSValueConst exc, const char* prefix = "Uncaught");

// Takes the pending exception from ctx and prints it.
void dump_pending_exception(JSContext* ctx);

// Throws a JS Error with the given message and returns JS_EXCEPTION.
JSValue throw_error(JSContext* ctx, const std::string& msg);

// Throws a Node-style system error, e.g.
//   Error: ENOENT: no such file or directory, open 'a.txt'
//   { code: 'ENOENT', errno: -2, syscall: 'open', path: 'a.txt' }
JSValue throw_errno(JSContext* ctx, int err, const char* syscall, const std::string& path = "");

// "ENOENT" for ENOENT, etc.
const char* errno_name(int err);

}  // namespace rtn
