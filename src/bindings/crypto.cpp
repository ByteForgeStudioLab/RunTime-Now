// Random bytes for crypto.getRandomValues() / crypto.randomUUID() (src/js/crypto.js).

#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

#include "bindings/bindings.hpp"

namespace rtn {

namespace {

// Fills `buf` from the kernel's CSPRNG. Returns false only if no source works.
bool random_bytes(uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom(buf + off, len - off, 0);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno == ENOSYS) {  // very old kernel
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd < 0) return false;
            while (off < len) {
                ssize_t r = read(fd, buf + off, len - off);
                if (r > 0) off += static_cast<size_t>(r);
                else if (r < 0 && errno == EINTR) continue;
                else break;
            }
            close(fd);
            return off == len;
        } else {
            return false;
        }
    }
    return true;
}

// randomFill(Uint8Array): fills it in place.
JSValue js_random_fill(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    size_t size = 0;
    uint8_t* bytes = argc > 0 && JS_GetTypedArrayType(argv[0]) >= 0 ? JS_GetUint8Array(ctx, &size, argv[0]) : nullptr;
    if (!bytes) return JS_ThrowTypeError(ctx, "randomFill: expected a Uint8Array");
    if (!random_bytes(bytes, size)) return JS_ThrowInternalError(ctx, "no source of random numbers available");
    return JS_UNDEFINED;
}

}  // namespace

void add_crypto_natives(JSContext* ctx, JSValueConst native) {
    JS_SetPropertyStr(ctx, native, "randomFill", JS_NewCFunction(ctx, js_random_fill, "randomFill", 1));
}

}  // namespace rtn
