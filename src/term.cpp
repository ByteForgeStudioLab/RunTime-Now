#include "term.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rtn::term {

namespace {

bool g_color = false;
bool g_truecolor = false;
bool g_fancy = false;

// Cyan -> violet -> pink.
constexpr Rgb kStops[] = {{34, 211, 238}, {129, 140, 248}, {192, 132, 252}, {244, 114, 182}};
constexpr int kStopCount = sizeof(kStops) / sizeof(kStops[0]);

Rgb mix(Rgb a, Rgb b, double t) {
    auto lerp = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b)};
}

// 0 -> 1 -> 0 as `phase` goes around, so a moving gradient has no seam.
double wave(double t) {
    t = t - std::floor(t);
    return t < 0.5 ? t * 2 : 2 - t * 2;
}

}  // namespace

void init(bool is_tty) {
    const char* term = std::getenv("TERM");
    const char* no_color = std::getenv("NO_COLOR");
    const char* ci = std::getenv("CI");
    g_color = is_tty && !(no_color && *no_color) && !(term && std::strcmp(term, "dumb") == 0);
    g_fancy = g_color && !(ci && *ci);
    const char* ct = std::getenv("COLORTERM");
    g_truecolor = ct && (std::strstr(ct, "truecolor") || std::strstr(ct, "24bit"));
}

bool color() { return g_color; }
bool fancy() { return g_fancy; }

int columns() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

std::string fg(Rgb c) {
    if (!g_color) return "";
    char buf[32];
    if (g_truecolor) {
        std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
    } else {  // nearest color in the xterm 6x6x6 cube
        auto q = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
        std::snprintf(buf, sizeof buf, "\x1b[38;5;%dm", 16 + 36 * q(c.r) + 6 * q(c.g) + q(c.b));
    }
    return buf;
}

const char* reset() { return g_color ? "\x1b[0m" : ""; }
const char* dim() { return g_color ? "\x1b[2m" : ""; }
const char* bold() { return g_color ? "\x1b[1m" : ""; }

Rgb gradient(double t) {
    t = std::clamp(t, 0.0, 1.0) * (kStopCount - 1);
    int i = std::min(static_cast<int>(t), kStopCount - 2);
    return mix(kStops[i], kStops[i + 1], t - i);
}

std::string gradient_text(std::string_view s, double phase) {
    if (!g_color) return std::string(s);
    size_t n = std::max<size_t>(visible_width(s), 1);
    std::string out;
    size_t i = 0;
    for (size_t k = 0; k < s.size();) {
        size_t len = 1;  // copy whole UTF-8 sequences
        while (k + len < s.size() && (static_cast<unsigned char>(s[k + len]) & 0xC0) == 0x80) ++len;
        out += fg(gradient(wave(static_cast<double>(i++) / n * 0.5 + phase)));
        out.append(s.substr(k, len));
        k += len;
    }
    return out + reset();
}

std::string spinner(int frame) {
    static const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return fg(gradient(wave(frame * 0.04))) + frames[frame % 10] + reset();
}

std::string progress_bar(double fraction, int width, int frame) {
    const Rgb track = {64, 64, 80};
    std::string out;
    if (!g_color) {  // plain ASCII (only used if someone forces it)
        int full = fraction < 0 ? 0 : static_cast<int>(fraction * width);
        return "[" + std::string(full, '#') + std::string(width - full, '-') + "]";
    }
    if (fraction < 0) {  // unknown size: a glowing segment sweeps back and forth
        const int seg = std::max(4, width / 4);
        int span = width - seg;
        int pos = span > 0 ? static_cast<int>(std::lround(wave(frame / 40.0) * span)) : 0;
        for (int i = 0; i < width; ++i) {
            bool lit = i >= pos && i < pos + seg;
            out += fg(lit ? gradient(static_cast<double>(i - pos) / seg) : track) + "━";
        }
        return out + reset();
    }
    fraction = std::clamp(fraction, 0.0, 1.0);
    double filled = fraction * width;
    int full = static_cast<int>(filled);
    bool half = filled - full >= 0.5 && full < width;
    // A highlight that runs along the filled part, like light on brushed metal.
    double shine = std::fmod(frame * 0.9, full + 12.0) - 6.0;
    for (int i = 0; i < full; ++i) {
        Rgb c = gradient(static_cast<double>(i) / std::max(width - 1, 1));
        double d = std::abs(i - shine);
        if (d < 3) c = mix(c, {255, 255, 255}, (1 - d / 3) * 0.55);
        out += fg(c) + "━";
    }
    int i = full;
    if (half) {
        out += fg(gradient(static_cast<double>(i) / std::max(width - 1, 1))) + "╸";
        ++i;
    }
    if (i < width) {
        out += fg(track);
        for (; i < width; ++i) out += "━";
    }
    return out + reset();
}

std::string format_bytes(double n) {
    char buf[32];
    if (n < 1024) std::snprintf(buf, sizeof buf, "%.0f B", n);
    else if (n < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", n / 1024);
    else std::snprintf(buf, sizeof buf, "%.1f MB", n / (1024 * 1024));
    return buf;
}

std::string format_seconds(double s) {
    char buf[32];
    if (s < 60) std::snprintf(buf, sizeof buf, "%.1fs", s);
    else std::snprintf(buf, sizeof buf, "%dm %02ds", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return buf;
}

size_t visible_width(std::string_view s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {  // skip "ESC [ ... letter"
            i += 2;
            while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
            continue;
        }
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++w;
    }
    return w;
}

void redraw_line(const std::string& s) {
    std::fputs("\r\x1b[2K", stdout);
    std::fputs(s.c_str(), stdout);
    std::fflush(stdout);
}

void hide_cursor() {
    if (g_fancy) std::fputs("\x1b[?25l", stdout);
}

void show_cursor() {
    if (g_fancy) {
        std::fputs("\x1b[?25h", stdout);
        std::fflush(stdout);
    }
}

std::string box(const std::vector<std::string>& lines, int frame) {
    size_t inner = 0;
    for (auto& l : lines) inner = std::max(inner, visible_width(l));
    inner += 4;  // two spaces of padding on each side
    const int perimeter = static_cast<int>(inner) * 2 + 4;
    int k = 0;  // position along the border, so the colors flow around the box
    auto border = [&](const char* ch) {
        return fg(gradient(wave(static_cast<double>(k++) / perimeter + frame * 0.03))) + ch;
    };
    std::string out = "  " + border("╭");
    for (size_t i = 0; i < inner; ++i) out += border("─");
    out += border("╮") + reset() + "\n";
    for (auto& l : lines) {
        out += "  " + border("│") + reset() + "  " + l;
        out += std::string(inner - 2 - visible_width(l), ' ');
        out += border("│") + reset() + "\n";
    }
    out += "  " + border("╰");
    for (size_t i = 0; i < inner; ++i) out += border("─");
    out += border("╯") + reset() + "\n";
    return out;
}

}  // namespace rtn::term
