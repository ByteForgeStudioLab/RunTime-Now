#pragma once

// Terminal UI helpers for `rtn upgrade`: colors with a gradient, a spinner,
// an animated progress bar and a box. Everything returns strings; nothing
// here decides *whether* to animate (see term::init()).

#include <string>
#include <string_view>
#include <vector>

namespace rtn::term {

struct Rgb {
    int r, g, b;
};

// Detects color support. `fancy` = animate (a real terminal, not CI, no NO_COLOR).
void init(bool is_tty);
bool color();
bool fancy();
int columns();  // terminal width (80 if unknown)

std::string fg(Rgb c);          // foreground color escape (truecolor or 256-color)
const char* reset();
const char* dim();
const char* bold();
Rgb gradient(double t);         // brand gradient, t in [0, 1]
// Each character gets its own color; `phase` shifts the gradient (for animation).
std::string gradient_text(std::string_view s, double phase = 0);

std::string spinner(int frame);
// fraction < 0: unknown size (a pulse sweeps back and forth).
std::string progress_bar(double fraction, int width, int frame);

std::string format_bytes(double n);     // "2.9 MB"
std::string format_seconds(double s);   // "0.8s", "1m 05s"
size_t visible_width(std::string_view s);  // ignores escape codes; counts code points

// Replaces the current line (no newline).
void redraw_line(const std::string& s);
void hide_cursor();
void show_cursor();
// A rounded box around `lines` with a gradient border. `frame` animates the border.
std::string box(const std::vector<std::string>& lines, int frame);

}  // namespace rtn::term
