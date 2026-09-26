// UTF-8 <-> JS string conversion for TextEncoder / TextDecoder (WHATWG rules).

#include <string>

#include "bindings/bindings.hpp"
#include "util.hpp"

namespace rtn {

namespace {

// Copies UTF-8, replacing invalid sequences with U+FFFD ("maximal subpart" rule).
// With `fatal`, returns false at the first invalid byte instead.
bool sanitize_utf8(const uint8_t* p, size_t n, bool fatal, std::string& out) {
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) {
            out += static_cast<char>(b);
            ++i;
            continue;
        }
        int need = 0;
        uint8_t lower = 0x80, upper = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2;
            if (b == 0xE0) lower = 0xA0;
            if (b == 0xED) upper = 0x9F;  // no surrogates
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3;
            if (b == 0xF0) lower = 0x90;
            if (b == 0xF4) upper = 0x8F;  // max U+10FFFF
        }
        size_t j = i + 1;
        bool bad = need == 0;
        for (int k = 0; k < need && !bad; ++k, ++j) {
            if (j >= n || p[j] < lower || p[j] > upper) {
                bad = true;
                break;
            }
            lower = 0x80;
            upper = 0xBF;
        }
        if (bad) {
            if (fatal) return false;
            out += "\xEF\xBF\xBD";
            i = (need == 0) ? i + 1 : j;
            continue;
        }
        out.append(reinterpret_cast<const char*>(p + i), j - i);
        i = j;
    }
    return true;
}

bool get_bytes(JSContext* ctx, JSValueConst v, const uint8_t*& data, size_t& size) {
    // Check types first: failed lookups throw internally, which is slow.
    if (JS_GetTypedArrayType(v) >= 0) {
        if (uint8_t* u8 = JS_GetUint8Array(ctx, &size, v)) {
            data = u8;
            return true;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (JS_IsArrayBuffer(v)) {
        if (uint8_t* ab = JS_GetArrayBuffer(ctx, &size, v)) {
            data = ab;
            return true;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return false;
}

// utf8Encode(string) -> Uint8Array
JSValue js_utf8_encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    size_t len = 0;
    const char* s = JS_ToCStringLen2(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED, false);
    if (!s) return JS_EXCEPTION;
    std::string bytes(s, len);
    JS_FreeCString(ctx, s);
    // Lone surrogates come out as ED A0..BF xx; the Encoding spec wants U+FFFD.
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (static_cast<uint8_t>(bytes[i]) == 0xED && static_cast<uint8_t>(bytes[i + 1]) >= 0xA0) {
            bytes.replace(i, 3, "\xEF\xBF\xBD");
            i += 2;
        }
    }
    return JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

// utf8Decode(Uint8Array | ArrayBuffer, fatal) -> string
JSValue js_utf8_decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (argc < 1 || !get_bytes(ctx, argv[0], data, size)) {
        return JS_ThrowTypeError(ctx, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
    }
    bool fatal = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    std::string text;
    if (!sanitize_utf8(data, size, fatal, text)) {
        return JS_ThrowTypeError(ctx, "The encoded data was not valid for encoding utf-8");
    }
    return JS_NewStringLen(ctx, text.data(), text.size());
}

const JSCFunctionListEntry kEncodingFuncs[] = {
    JS_CFUNC_DEF("utf8Encode", 1, js_utf8_encode),
    JS_CFUNC_DEF("utf8Decode", 2, js_utf8_decode),
};

}  // namespace

void add_encoding_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyFunctionList(ctx, native, kEncodingFuncs, sizeof(kEncodingFuncs) / sizeof(kEncodingFuncs[0]));
}

}  // namespace rtn
