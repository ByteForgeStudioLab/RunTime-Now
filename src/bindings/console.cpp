// console.log / error / warn / info / debug / time / timeEnd / assert
// plus `inspect`, which turns any JS value into readable text like Node does.

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "bindings/bindings.hpp"
#include "runtime.hpp"
#include "util.hpp"

namespace rtn {

namespace {

constexpr int kMaxDepth = 2;

// Objects can customise how they print by defining a method under
// Symbol.for("rtn.inspect") (used by URL, Headers, Request, Response...).
JSAtom g_inspect_atom = JS_ATOM_NULL;

// Drops stack frames that point into rtn's own embedded JS.
std::string clean_stack(const std::string& stack) {
    std::string out;
    size_t p = 0;
    while (p < stack.size()) {
        size_t e = stack.find('\n', p);
        if (e == std::string::npos) e = stack.size();
        std::string_view line(stack.data() + p, e - p);
        if (line.find("rtn:internal/") == std::string_view::npos) {
            out.append(line);
            out += '\n';
        }
        p = e + 1;
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
constexpr uint32_t kMaxArrayItems = 100;
constexpr size_t kLineWidth = 72;

struct Style {
    bool on;
    std::string wrap(const char* code, const std::string& s) const {
        return on ? std::string("\x1b[") + code + "m" + s + "\x1b[0m" : s;
    }
    std::string num(const std::string& s) const { return wrap("33", s); }
    std::string str(const std::string& s) const { return wrap("32", s); }
    std::string special(const std::string& s) const { return wrap("36", s); }
    std::string dim(const std::string& s) const { return wrap("90", s); }
    std::string bold(const std::string& s) const { return wrap("1", s); }
    std::string magenta(const std::string& s) const { return wrap("35", s); }
    std::string red(const std::string& s) const { return wrap("31", s); }
};

size_t visible_length(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b') {
            while (i < s.size() && s[i] != 'm') ++i;
        } else if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            ++n;  // count UTF-8 code points, not bytes
        }
    }
    return n;
}

std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        switch (c) {
            case '\'': out += "\\'"; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out + "'";
}

bool is_identifier(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$') return false;
    }
    return true;
}

std::string get_string_prop(JSContext* ctx, JSValueConst obj, const char* name) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    std::string s = JS_IsString(v) ? to_string(ctx, v) : "";
    JS_FreeValue(ctx, v);
    return s;
}

// obj.constructor.name, or "" if there is none.
std::string constructor_name(JSContext* ctx, JSValueConst obj) {
    JSValue proto = JS_GetPrototype(ctx, obj);
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return "";
    }
    if (JS_IsNull(proto)) return "[Object: null prototype]";
    JSValue ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    std::string name = JS_IsFunction(ctx, ctor) ? get_string_prop(ctx, ctor, "name") : "";
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, proto);
    return name;
}

class Inspector {
public:
    Inspector(JSContext* ctx, bool colors) : ctx_(ctx), st_{colors} {}

    std::string format(JSValueConst v, int depth, int indent) {
        if (JS_IsUndefined(v)) return st_.dim("undefined");
        if (JS_IsNull(v)) return st_.bold("null");
        if (JS_IsBool(v)) return st_.num(JS_ToBool(ctx_, v) ? "true" : "false");
        if (JS_IsNumber(v)) {
            double d = 0;
            JS_ToFloat64(ctx_, &d, v);
            if (d == 0 && std::signbit(d)) return st_.num("-0");
            return st_.num(to_string(ctx_, v));
        }
        if (JS_IsBigInt(v)) return st_.num(to_string(ctx_, v) + "n");
        if (JS_IsString(v)) return st_.str(quote(to_string(ctx_, v)));
        if (JS_IsSymbol(v)) return st_.str("Symbol(" + get_string_prop(ctx_, v, "description") + ")");
        if (!JS_IsObject(v)) return to_string(ctx_, v);

        if (JS_IsFunction(ctx_, v)) return st_.special(format_function(v));
        if (JS_IsError(v) || constructor_name(ctx_, v) == "DOMException") return format_error(v, depth, indent);
        if (JS_IsRegExp(v)) return st_.red(to_string(ctx_, v));
        if (JS_IsDate(v)) return st_.magenta(call_method_string(v, "toISOString"));

        void* ptr = JS_VALUE_GET_PTR(v);
        if (seen_.contains(ptr)) {  // like Node: <ref *1> { self: [Circular *1] }
            auto [it, added] = circular_ids_.try_emplace(ptr, static_cast<int>(circular_ids_.size()) + 1);
            return st_.special("[Circular *" + std::to_string(it->second) + "]");
        }
        if (depth > kMaxDepth) {  // like Node: [Array], [Object], [Map], [MyClass]
            if (JS_IsArray(v)) return st_.special("[Array]");
            std::string name = constructor_name(ctx_, v);
            return st_.special("[" + (name.empty() || name.starts_with("[") ? std::string("Object") : name) + "]");
        }

        seen_.insert(ptr);
        std::string out = format_custom(v, depth, indent);
        if (out.empty()) out = format_object(v, depth, indent);
        seen_.erase(ptr);
        if (auto it = circular_ids_.find(ptr); it != circular_ids_.end()) {
            out = st_.special("<ref *" + std::to_string(it->second) + ">") + " " + out;
        }
        return out;
    }

    // [Function: name]  [Function (anonymous)]  [class Foo]  [class Bar extends Foo]
    std::string format_function(JSValueConst fn) {
        std::string name = get_string_prop(ctx_, fn, "name");
        bool is_class = false;
        if (JS_IsConstructor(ctx_, fn)) {
            JSValue src = JS_Eval(ctx_, "Function.prototype.toString", 27, "<rtn>", JS_EVAL_TYPE_GLOBAL);
            JSValue text = JS_Call(ctx_, src, fn, 0, nullptr);
            is_class = JS_IsString(text) && to_string(ctx_, text).starts_with("class");
            if (JS_IsException(text)) JS_FreeValue(ctx_, JS_GetException(ctx_));
            JS_FreeValue(ctx_, text);
            JS_FreeValue(ctx_, src);
        }
        if (!is_class) return name.empty() ? "[Function (anonymous)]" : "[Function: " + name + "]";
        std::string out = "[class " + (name.empty() ? std::string("(anonymous)") : name);
        JSValue parent = JS_GetPrototype(ctx_, fn);
        if (JS_IsFunction(ctx_, parent)) {
            std::string pname = get_string_prop(ctx_, parent, "name");
            if (!pname.empty()) out += " extends " + pname;
        }
        JS_FreeValue(ctx_, parent);
        return out + "]";
    }

    // Error: message\n    at ...  { code: 'ENOENT', [cause]: ... }
    std::string format_error(JSValueConst err, int depth = 0, int indent = 0) {
        std::string text = JS_IsError(err)  // else a DOMException, shown the way Node does
            ? to_string(ctx_, err)
            : "DOMException [" + get_string_prop(ctx_, err, "name") + "]: " + get_string_prop(ctx_, err, "message");
        std::string stack = clean_stack(get_string_prop(ctx_, err, "stack"));
        if (!stack.empty()) text += "\n" + stack;
        if (depth > kMaxDepth) return text;

        std::vector<std::string> parts;
        void* ptr = JS_VALUE_GET_PTR(err);
        seen_.insert(ptr);
        add_own_props(parts, err, depth, indent, false, {"stack", "message"});
        JSValue cause = JS_GetPropertyStr(ctx_, err, "cause");
        if (!JS_IsUndefined(cause) && !JS_IsException(cause)) {
            parts.push_back("[cause]: " + format(cause, depth + 1, indent + 2));
        }
        JS_FreeValue(ctx_, cause);
        seen_.erase(ptr);
        if (!parts.empty()) {
            std::string pad(indent + 2, ' ');
            text += " {\n";
            for (size_t i = 0; i < parts.size(); ++i) text += pad + parts[i] + (i + 1 < parts.size() ? ",\n" : "\n");
            text += std::string(indent, ' ') + "}";
        }
        return text;
    }

private:
    std::string call_method_string(JSValueConst obj, const char* method) {
        JSValue fn = JS_GetPropertyStr(ctx_, obj, method);
        JSValue r = JS_Call(ctx_, fn, obj, 0, nullptr);
        std::string s = JS_IsException(r) ? "Invalid Date" : to_string(ctx_, r);
        if (JS_IsException(r)) JS_FreeValue(ctx_, JS_GetException(ctx_));
        JS_FreeValue(ctx_, r);
        JS_FreeValue(ctx_, fn);
        return s;
    }

    uint32_t length_of(JSValueConst obj) {
        JSValue len = JS_GetPropertyStr(ctx_, obj, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx_, &n, len);
        JS_FreeValue(ctx_, len);
        return n;
    }

    // Array.from(iterable) — used for Map and Set.
    JSValue array_from(JSValueConst iterable) {
        JSValue global = JS_GetGlobalObject(ctx_);
        JSValue array = JS_GetPropertyStr(ctx_, global, "Array");
        JSValue from = JS_GetPropertyStr(ctx_, array, "from");
        JSValue r = JS_Call(ctx_, from, array, 1, &iterable);
        JS_FreeValue(ctx_, from);
        JS_FreeValue(ctx_, array);
        JS_FreeValue(ctx_, global);
        return r;
    }

    void add_items(std::vector<std::string>& parts, JSValueConst arr, int depth, int indent,
                   bool map_entries) {
        uint32_t n = length_of(arr);
        for (uint32_t i = 0; i < n && i < kMaxArrayItems; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx_, arr, i);
            if (map_entries) {
                JSValue k = JS_GetPropertyUint32(ctx_, item, 0);
                JSValue val = JS_GetPropertyUint32(ctx_, item, 1);
                parts.push_back(format(k, depth + 1, indent + 2) + " => " +
                                format(val, depth + 1, indent + 2));
                JS_FreeValue(ctx_, k);
                JS_FreeValue(ctx_, val);
            } else {
                parts.push_back(format(item, depth + 1, indent + 2));
            }
            JS_FreeValue(ctx_, item);
        }
        if (n > kMaxArrayItems) {
            parts.push_back("... " + std::to_string(n - kMaxArrayItems) + " more items");
        }
    }

    // Uses obj[Symbol.for("rtn.inspect")]() if present (a string is printed as is). Returns "" if not.
    std::string format_custom(JSValueConst v, int depth, int indent) {
        if (g_inspect_atom == JS_ATOM_NULL) return "";
        JSValue fn = JS_GetProperty(ctx_, v, g_inspect_atom);
        if (!JS_IsFunction(ctx_, fn)) {
            JS_FreeValue(ctx_, fn);
            return "";
        }
        JSValue shown = JS_Call(ctx_, fn, v, 0, nullptr);
        JS_FreeValue(ctx_, fn);
        if (JS_IsException(shown)) {
            JS_FreeValue(ctx_, JS_GetException(ctx_));
            return "";
        }
        if (JS_IsString(shown)) {  // a ready-made representation, e.g. "<Buffer 68 69>"
            std::string raw = to_string(ctx_, shown);
            JS_FreeValue(ctx_, shown);
            return raw;
        }
        std::string name = constructor_name(ctx_, v);
        std::string body = JS_IsObject(shown) ? format_object(shown, depth, indent) : format(shown, depth + 1, indent);
        JS_FreeValue(ctx_, shown);
        return name.empty() ? body : name + " " + body;
    }

    void add_own_props(std::vector<std::string>& parts, JSValueConst obj, int depth, int indent,
                       bool skip_indices, std::initializer_list<std::string_view> skip = {}) {
        JSPropertyEnum* tab = nullptr;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx_, &tab, &len, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreeValue(ctx_, JS_GetException(ctx_));
            return;
        }
        for (uint32_t i = 0; i < len; ++i) {
            const char* key_c = JS_AtomToCString(ctx_, tab[i].atom);
            std::string key = key_c ? key_c : "";
            JS_FreeCString(ctx_, key_c);
            if (skip_indices && !key.empty() && std::isdigit(static_cast<unsigned char>(key[0]))) continue;
            bool skipped = false;
            for (auto k : skip) skipped |= key == k;
            if (skipped) continue;

            JSValue val = JS_GetProperty(ctx_, obj, tab[i].atom);
            std::string shown_key = is_identifier(key) ? key : st_.str(quote(key));
            parts.push_back(shown_key + ": " + format(val, depth + 1, indent + 2));
            JS_FreeValue(ctx_, val);
        }
        JS_FreePropertyEnum(ctx_, tab, len);
    }

    std::string join(const std::string& prefix, const char* open, const char* close,
                     const std::vector<std::string>& parts, int indent) {
        if (parts.empty()) return prefix + open + close;
        size_t total = prefix.size() + indent;
        bool multiline = false;
        for (const auto& p : parts) {
            total += visible_length(p) + 2;
            if (p.find('\n') != std::string::npos) multiline = true;
        }
        if (!multiline && total <= kLineWidth) {
            std::string out = prefix + open + " ";
            for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
            return out + " " + close;
        }
        std::string pad(indent + 2, ' ');
        std::string out = prefix + open + "\n";
        for (size_t i = 0; i < parts.size(); ++i) {
            out += pad + parts[i] + (i + 1 < parts.size() ? ",\n" : "\n");
        }
        return out + std::string(indent, ' ') + close;
    }

    std::string format_object(JSValueConst v, int depth, int indent) {
        std::vector<std::string> parts;

        if (JS_IsArray(v)) {
            add_items(parts, v, depth, indent, false);
            add_own_props(parts, v, depth, indent, true);
            return join("", "[", "]", parts, indent);
        }
        if (JS_GetTypedArrayType(v) >= 0) {
            add_items(parts, v, depth, indent, false);
            std::string name = constructor_name(ctx_, v);
            return join(name + "(" + std::to_string(length_of(v)) + ") ", "[", "]", parts, indent);
        }
        if (JS_IsMap(v) || JS_IsSet(v)) {
            bool is_map = JS_IsMap(v);
            JSValue arr = array_from(v);
            if (!JS_IsException(arr)) add_items(parts, arr, depth, indent, is_map);
            std::string prefix = (is_map ? "Map(" : "Set(") + std::to_string(length_of(arr)) + ") ";
            JS_FreeValue(ctx_, arr);
            return join(prefix, "{", "}", parts, indent);
        }
        if (JS_IsPromise(v)) {
            switch (JS_PromiseState(ctx_, v)) {
                case JS_PROMISE_PENDING: parts.push_back(st_.special("<pending>")); break;
                default: {
                    JSValue r = JS_PromiseResult(ctx_, v);
                    std::string s = format(r, depth + 1, indent + 2);
                    if (JS_PromiseState(ctx_, v) == JS_PROMISE_REJECTED) s = st_.red("<rejected> ") + s;
                    parts.push_back(s);
                    JS_FreeValue(ctx_, r);
                }
            }
            return join("Promise ", "{", "}", parts, indent);
        }

        add_own_props(parts, v, depth, indent, false);
        std::string name = constructor_name(ctx_, v);
        std::string prefix = (name.empty() || name == "Object") ? "" : name + " ";
        return join(prefix, "{", "}", parts, indent);
    }

    JSContext* ctx_;
    Style st_;
    std::set<void*> seen_;
    std::map<void*, int> circular_ids_;
};

// console.log("%s is %d years", "Ali", 20) style formatting.
std::string format_args(JSContext* ctx, int argc, JSValueConst* argv, bool colors) {
    std::string out;
    int next = 0;

    if (argc > 0 && JS_IsString(argv[0])) {
        std::string fmt = to_string(ctx, argv[0]);
        next = 1;
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) {
                out += fmt[i];
                continue;
            }
            char c = fmt[i + 1];
            if (c == '%') {
                out += '%';
                ++i;
                continue;
            }
            if (std::string_view("sdifoOjc").find(c) == std::string_view::npos || next >= argc) {
                out += fmt[i];
                continue;
            }
            JSValueConst a = argv[next++];
            ++i;
            switch (c) {
                case 's': out += JS_IsString(a) ? to_string(ctx, a) : Inspector(ctx, false).format(a, 1, 0); break;
                case 'd':
                case 'i': {
                    double d = 0;
                    JS_ToFloat64(ctx, &d, a);
                    out += (c == 'i') ? std::to_string(static_cast<int64_t>(d)) : to_string(ctx, a);
                    break;
                }
                case 'f': {
                    double d = 0;
                    JS_ToFloat64(ctx, &d, a);
                    JSValue n = JS_NewFloat64(ctx, d);
                    out += to_string(ctx, n);
                    break;
                }
                case 'j': {
                    JSValue json = JS_JSONStringify(ctx, a, JS_UNDEFINED, JS_UNDEFINED);
                    out += JS_IsException(json) ? "[Circular]" : to_string(ctx, json);
                    if (JS_IsException(json)) JS_FreeValue(ctx, JS_GetException(ctx));
                    JS_FreeValue(ctx, json);
                    break;
                }
                case 'c': break;  // CSS styles: ignored in a terminal
                default: out += Inspector(ctx, colors).format(a, 0, 0);
            }
        }
    }

    for (int i = next; i < argc; ++i) {
        if (!out.empty() || i > 0) out += ' ';
        if (JS_IsString(argv[i])) {
            out += to_string(ctx, argv[i]);  // top-level strings are printed raw
        } else {
            out += Inspector(ctx, colors).format(argv[i], 0, 0);
        }
    }
    return out;
}

int g_group_indent = 0;  // console.group() nesting, in spaces

// Writes text (indented for console.group) and flushes, like Node does.
void emit(FILE* out, const std::string& text) {
    std::string pad(static_cast<size_t>(g_group_indent), ' ');
    std::string line = pad;
    for (char c : text) {
        line += c;
        if (c == '\n') line += pad;
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), out);
    std::fflush(out);  // logs must not be lost if the process is killed
}

// magic: 0 = stdout, 1 = stderr
JSValue js_console_print(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    FILE* out = magic ? stderr : stdout;
    emit(out, format_args(ctx, argc, argv, isatty(fileno(out))));
    return JS_UNDEFINED;
}

JSValue js_console_dir(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    emit(stdout, argc > 0 ? inspect(ctx, argv[0], isatty(STDOUT_FILENO)) : "undefined");
    return JS_UNDEFINED;
}

JSValue js_console_assert(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc > 0 && JS_ToBool(ctx, argv[0])) return JS_UNDEFINED;
    std::string msg = "Assertion failed";
    if (argc > 1) msg += ": " + format_args(ctx, argc - 1, argv + 1, false);
    emit(stderr, msg);
    return JS_UNDEFINED;
}

// console.trace(...data): the message plus the current stack, on stderr.
JSValue js_console_trace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string msg = "Trace";
    if (argc > 0) msg += ": " + format_args(ctx, argc, argv, false);
    JSValue err = JS_NewError(ctx);
    std::string stack = clean_stack(get_string_prop(ctx, err, "stack"));
    JS_FreeValue(ctx, err);
    // drop the "at trace (native)" frame
    if (stack.find("(native)") != std::string::npos) {
        size_t nl = stack.find('\n');
        stack = nl == std::string::npos ? "" : stack.substr(nl + 1);
    }
    emit(stderr, stack.empty() ? msg : msg + "\n" + stack);
    return JS_UNDEFINED;
}

std::string label_of(JSContext* ctx, int argc, JSValueConst* argv) {
    return (argc > 0 && !JS_IsUndefined(argv[0])) ? to_string(ctx, argv[0]) : "default";
}

std::map<std::string, int>& counters() {
    static std::map<std::string, int> c;
    return c;
}

// magic: 0 = count, 1 = countReset
JSValue js_console_count(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string label = label_of(ctx, argc, argv);
    if (magic == 1) {
        counters()[label] = 0;
    } else {
        emit(stdout, label + ": " + std::to_string(++counters()[label]));
    }
    return JS_UNDEFINED;
}

// magic: 0 = group / groupCollapsed, 1 = groupEnd
JSValue js_console_group(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    if (magic == 1) {
        g_group_indent = std::max(0, g_group_indent - 2);
        return JS_UNDEFINED;
    }
    if (argc > 0) emit(stdout, format_args(ctx, argc, argv, isatty(STDOUT_FILENO)));
    g_group_indent += 2;
    return JS_UNDEFINED;
}

std::map<std::string, std::chrono::steady_clock::time_point>& timers() {
    static std::map<std::string, std::chrono::steady_clock::time_point> t;
    return t;
}

JSValue js_console_time(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    timers()[label_of(ctx, argc, argv)] = std::chrono::steady_clock::now();
    return JS_UNDEFINED;
}

// magic: 0 = timeEnd, 1 = timeLog
JSValue js_console_time_end(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string label = label_of(ctx, argc, argv);
    auto it = timers().find(label);
    if (it == timers().end()) {
        emit(stderr, "Warning: No such label '" + label + "' for console." + (magic ? "timeLog()" : "timeEnd()"));
        return JS_UNDEFINED;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - it->second).count();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3fms", ms);
    std::string line = label + ": " + buf;
    if (magic == 0) {
        timers().erase(it);
    } else if (argc > 1) {
        line += " " + format_args(ctx, argc - 1, argv + 1, isatty(STDOUT_FILENO));
    }
    emit(stdout, line);
    return JS_UNDEFINED;
}

// console.table(data, columns?)
JSValue js_console_table(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsFunction(ctx, argv[0])) {
        emit(stdout, format_args(ctx, argc, argv, isatty(STDOUT_FILENO)));
        return JS_UNDEFINED;
    }
    auto own_keys = [&](JSValueConst obj) {
        std::vector<std::string> keys;
        JSPropertyEnum* tab = nullptr;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < len; ++i) {
                const char* k = JS_AtomToCString(ctx, tab[i].atom);
                keys.emplace_back(k ? k : "");
                JS_FreeCString(ctx, k);
            }
            JS_FreePropertyEnum(ctx, tab, len);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        return keys;
    };
    auto cell = [&](JSValueConst v) { return Inspector(ctx, false).format(v, 1, 0); };

    std::vector<std::string> row_keys = own_keys(argv[0]);
    std::vector<std::string> columns;
    bool has_values = false;
    std::vector<std::map<std::string, std::string>> rows;
    std::vector<std::string> values;
    for (const auto& rk : row_keys) {
        JSValue row = JS_GetPropertyStr(ctx, argv[0], rk.c_str());
        std::map<std::string, std::string> cells;
        std::string value;
        if (JS_IsObject(row) && !JS_IsFunction(ctx, row)) {
            for (const auto& ck : own_keys(row)) {
                if (std::find(columns.begin(), columns.end(), ck) == columns.end()) columns.push_back(ck);
                JSValue c = JS_GetPropertyStr(ctx, row, ck.c_str());
                cells[ck] = cell(c);
                JS_FreeValue(ctx, c);
            }
        } else {
            has_values = true;
            value = cell(row);
        }
        JS_FreeValue(ctx, row);
        rows.push_back(std::move(cells));
        values.push_back(std::move(value));
    }
    if (argc > 1 && JS_IsArray(argv[1])) {  // explicit column filter
        columns.clear();
        for (const auto& k : own_keys(argv[1])) {
            JSValue c = JS_GetPropertyStr(ctx, argv[1], k.c_str());
            columns.push_back(to_string(ctx, c));
            JS_FreeValue(ctx, c);
        }
    }

    std::vector<std::string> header{"(index)"};
    header.insert(header.end(), columns.begin(), columns.end());
    if (has_values) header.push_back("Values");
    std::vector<std::vector<std::string>> table;
    for (size_t r = 0; r < rows.size(); ++r) {
        std::vector<std::string> line{row_keys[r]};
        for (const auto& c : columns) line.push_back(rows[r].count(c) ? rows[r][c] : "");
        if (has_values) line.push_back(values[r]);
        table.push_back(std::move(line));
    }
    std::vector<size_t> width(header.size());
    for (size_t c = 0; c < header.size(); ++c) {
        width[c] = visible_length(header[c]);
        for (const auto& line : table) width[c] = std::max(width[c], visible_length(line[c]));
    }
    auto border = [&](const char* l, const char* m, const char* r) {
        std::string s = l;
        for (size_t c = 0; c < width.size(); ++c) {
            for (size_t i = 0; i < width[c] + 2; ++i) s += "─";
            s += c + 1 < width.size() ? m : r;
        }
        return s;
    };
    auto render = [&](const std::vector<std::string>& line) {
        std::string s = "│";
        for (size_t c = 0; c < line.size(); ++c) {
            s += " " + line[c] + std::string(width[c] - visible_length(line[c]), ' ') + " │";
        }
        return s;
    };
    std::string out = border("┌", "┬", "┐") + "\n" + render(header) + "\n" + border("├", "┼", "┤") + "\n";
    for (const auto& line : table) out += render(line) + "\n";
    out += border("└", "┴", "┘");
    emit(stdout, out);
    return JS_UNDEFINED;
}

const JSCFunctionListEntry kConsoleFuncs[] = {
    JS_CFUNC_MAGIC_DEF("log", 1, js_console_print, 0),
    JS_CFUNC_MAGIC_DEF("info", 1, js_console_print, 0),
    JS_CFUNC_MAGIC_DEF("debug", 1, js_console_print, 0),
    JS_CFUNC_MAGIC_DEF("error", 1, js_console_print, 1),
    JS_CFUNC_MAGIC_DEF("warn", 1, js_console_print, 1),
    JS_CFUNC_DEF("dir", 1, js_console_dir),
    JS_CFUNC_DEF("assert", 2, js_console_assert),
    JS_CFUNC_DEF("trace", 1, js_console_trace),
    JS_CFUNC_MAGIC_DEF("count", 1, js_console_count, 0),
    JS_CFUNC_MAGIC_DEF("countReset", 1, js_console_count, 1),
    JS_CFUNC_MAGIC_DEF("group", 1, js_console_group, 0),
    JS_CFUNC_MAGIC_DEF("groupCollapsed", 1, js_console_group, 0),
    JS_CFUNC_MAGIC_DEF("groupEnd", 0, js_console_group, 1),
    JS_CFUNC_DEF("time", 1, js_console_time),
    JS_CFUNC_MAGIC_DEF("timeEnd", 1, js_console_time_end, 0),
    JS_CFUNC_MAGIC_DEF("timeLog", 1, js_console_time_end, 1),
    JS_CFUNC_DEF("table", 2, js_console_table),
};

}  // namespace

std::string inspect(JSContext* ctx, JSValueConst v, bool colors) {
    return Inspector(ctx, colors).format(v, 0, 0);
}

namespace {

// inspect(value, colors) -> what console.log would show for one value (strings quoted)
JSValue js_inspect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    bool colors = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    std::string s = inspect(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, colors);
    return JS_NewStringLen(ctx, s.data(), s.size());
}

// format(...args) -> the line console.log(...args) prints (util.format)
JSValue js_format(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string s = format_args(ctx, argc, argv, false);
    return JS_NewStringLen(ctx, s.data(), s.size());
}

}  // namespace

void add_console_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyStr(ctx, native, "inspect", JS_NewCFunction(ctx, js_inspect, "inspect", 2));
    JS_SetPropertyStr(ctx, native, "format", JS_NewCFunction(ctx, js_format, "format", 1));
}

void install_console(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, console, kConsoleFuncs,
                               sizeof(kConsoleFuncs) / sizeof(kConsoleFuncs[0]));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);

    // Symbol.for("rtn.inspect"), kept as an atom for fast lookups.
    JSValue sym = JS_Eval(ctx, "Symbol.for('rtn.inspect')", 25, "<rtn>", JS_EVAL_TYPE_GLOBAL);
    g_inspect_atom = JS_ValueToAtom(ctx, sym);
    JS_FreeValue(ctx, sym);
    Runtime::from(ctx)->on_shutdown([ctx] {
        JS_FreeAtom(ctx, g_inspect_atom);
        g_inspect_atom = JS_ATOM_NULL;
    });
}

}  // namespace rtn
