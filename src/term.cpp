#include "term.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
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

namespace {

enum class Key { Up, Down, Enter, Cancel, Other };

// Reads one key press in raw mode; `digit` gets '1'..'9' when one was typed.
Key read_key(char& digit) {
    digit = 0;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return Key::Cancel;  // EOF / error
    if (c == '\r' || c == '\n') return Key::Enter;
    if (c == 3 || c == 4) return Key::Cancel;  // Ctrl+C, Ctrl+D
    if (c == 'k' || c == 16) return Key::Up;    // k, Ctrl+P
    if (c == 'j' || c == 14) return Key::Down;  // j, Ctrl+N
    if (c >= '1' && c <= '9') {
        digit = static_cast<char>(c);
        return Key::Other;
    }
    if (c != 0x1b) return Key::Other;
    // A lone Esc cancels; "ESC [ A" / "ESC O A" are the arrow keys.
    pollfd p{STDIN_FILENO, POLLIN, 0};
    unsigned char seq[2];
    if (poll(&p, 1, 50) <= 0 || read(STDIN_FILENO, &seq[0], 1) != 1) return Key::Cancel;
    if (seq[0] != '[' && seq[0] != 'O') return Key::Other;
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return Key::Cancel;
    if (seq[1] == 'A') return Key::Up;
    if (seq[1] == 'B') return Key::Down;
    return Key::Other;
}

}  // namespace

int select(std::string_view question, const std::vector<Choice>& choices, int initial) {
    const int n = static_cast<int>(choices.size());
    if (n == 0) return -1;
    termios saved{};
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return initial;
    termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);  // Ctrl+C arrives as a key, so the terminal is always restored
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    size_t width = 0;
    for (auto& c : choices) width = std::max(width, visible_width(c.label));
    std::string a = accent(), r = reset(), d = dim(), b = bold();
    std::fputs("\x1b[?25l", stdout);  // hide the cursor while the menu is up
    std::string title = "  " + a + "?" + r + " " + b + std::string(question) + r;
    std::printf("%s %s↑/↓ to move, enter to select%s\n", title.c_str(), d.c_str(), r.c_str());

    int cur = std::clamp(initial, 0, n - 1);
    auto draw = [&](bool first) {
        if (!first) std::printf("\x1b[%dA", n);  // back to the first choice
        for (int i = 0; i < n; ++i) {
            const auto& c = choices[i];
            std::string pad(width - visible_width(c.label) + 3, ' ');
            std::string line = i == cur ? "  " + a + "❯ " + c.label + r : "    " + c.label;
            redraw_line(line + pad + d + c.hint + r + "\n");
        }
    };
    draw(true);
    int result = -1;
    for (;;) {
        char digit;
        Key k = read_key(digit);
        if (k == Key::Cancel) break;
        if (k == Key::Enter) {
            result = cur;
            break;
        }
        if (digit && digit - '1' < n) {
            result = digit - '1';
            break;
        }
        if (k == Key::Up) cur = (cur + n - 1) % n;
        else if (k == Key::Down) cur = (cur + 1) % n;
        draw(false);
    }

    // Collapse the menu into one line with the answer.
    std::printf("\x1b[%dA", n + 1);
    for (int i = 0; i <= n; ++i) std::fputs("\r\x1b[2K\n", stdout);
    std::printf("\x1b[%dA", n + 1);
    if (result >= 0) {
        std::printf("  %s✓%s %s%s%s %s%s%s\n", a.c_str(), r.c_str(), b.c_str(), std::string(question).c_str(),
                    r.c_str(), a.c_str(), choices[result].label.c_str(), r.c_str());
    } else {
        std::printf("  %s✗%s %s %sCancelled%s\n", d.c_str(), r.c_str(), std::string(question).c_str(), d.c_str(), r.c_str());
    }
    std::fputs("\x1b[?25h", stdout);
    std::fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    return result;
}

}  // namespace rtn::term
