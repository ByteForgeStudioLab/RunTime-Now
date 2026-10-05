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

// Tailwind blue-500 for everything that is "ours"; blue-300 for the highlight
// that glides along the progress bar. Text keeps the terminal's own color, so
// it reads well on dark and light backgrounds alike.
constexpr Rgb kBlue = {59, 130, 246};
constexpr Rgb kBlueLight = {147, 197, 253};

Rgb mix(Rgb a, Rgb b, double t) {
    auto lerp = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b)};
}

// 0 -> 1 -> 0 as `t` goes from 0 to 1 (smooth back-and-forth motion).
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

const char* accent() {
    static std::string blue;
    if (!g_color) return "";
    if (blue.empty()) blue = fg(kBlue);
    return blue.c_str();
}

std::string accent_text(std::string_view s) {
    return std::string(accent()) + std::string(s) + reset();
}

std::string spinner(int frame) {
    static const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return std::string(accent()) + frames[frame % 10] + reset();
}

std::string progress_bar(double fraction, int width, int frame) {
    if (!g_color) {  // plain ASCII (only used if someone forces it)
        int full = fraction < 0 ? 0 : static_cast<int>(fraction * width);
        return "[" + std::string(full, '#') + std::string(width - full, '-') + "]";
    }
    // The empty part of the track: the terminal's text color, dimmed (gray on any background).
    auto track = [&](int n) {
        std::string t = std::string(reset()) + dim();
        for (int i = 0; i < n; ++i) t += "━";
        return t + reset();
    };
    std::string out;
    if (fraction < 0) {  // unknown size: a blue segment glides back and forth
        const int seg = std::max(4, width / 5);
        int span = width - seg;
        int pos = span > 0 ? static_cast<int>(std::lround(wave(frame / 50.0) * span)) : 0;
        out += track(pos) + accent();
        for (int i = 0; i < seg; ++i) out += "━";
        return out + track(width - pos - seg);
    }
    fraction = std::clamp(fraction, 0.0, 1.0);
    double filled = fraction * width;
    int full = static_cast<int>(filled);
    bool half = filled - full >= 0.5 && full < width;
    // A soft light-blue glint travels along the filled part every couple of seconds.
    double glint = std::fmod(frame * 0.6, full + 24.0) - 4.0;
    for (int i = 0; i < full; ++i) {
        double d = std::abs(i - glint);
        out += d < 4 ? fg(mix(kBlueLight, kBlue, d / 4)) : accent();
        out += "━";
    }
    if (half) out += std::string(accent()) + "╸";
    return out + track(width - full - (half ? 1 : 0));
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

std::string box(const std::vector<std::string>& lines) {
    size_t inner = 0;
    for (auto& l : lines) inner = std::max(inner, visible_width(l));
    inner += 4;  // two spaces of padding on each side
    std::string bar;
    for (size_t i = 0; i < inner; ++i) bar += "─";
    std::string b = accent(), r = reset();
    std::string out = "  " + b + "╭" + bar + "╮" + r + "\n";
    for (auto& l : lines) {
        out += "  " + b + "│" + r + "  " + l + std::string(inner - 2 - visible_width(l), ' ') + b + "│" + r + "\n";
    }
    return out + "  " + b + "╰" + bar + "╯" + r + "\n";
}

}  // namespace rtn::term
