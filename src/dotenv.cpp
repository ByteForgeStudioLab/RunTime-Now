#include "dotenv.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>

#include "util.hpp"

namespace rtn::dotenv {

namespace {

bool is_key_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
}

bool is_name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// ${NAME} / $NAME -> the variable's value ("" if unset). "\$" is a literal "$".
std::string expand(std::string_view v, const std::vector<Entry>& before, const char* (*lookup)(const char*)) {
    auto value_of = [&](const std::string& name) -> std::string {
        if (const char* env = lookup(name.c_str())) return env;  // the environment wins, as when loading
        for (auto it = before.rbegin(); it != before.rend(); ++it) {
            if (it->key == name) return it->value;
        }
        return "";
    };
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        if (c == '\\' && i + 1 < v.size() && v[i + 1] == '$') {
            out += '$';
            ++i;
        } else if (c == '$' && i + 1 < v.size() && v[i + 1] == '{') {
            size_t end = v.find('}', i + 2);
            if (end == std::string_view::npos) {
                out += v.substr(i);
                break;
            }
            std::string name(v.substr(i + 2, end - i - 2));
            std::string fallback;
            if (size_t d = name.find(":-"); d != std::string::npos) {  // ${NAME:-default}
                fallback = name.substr(d + 2);
                name.resize(d);
            }
            std::string val = value_of(name);
            out += val.empty() ? fallback : val;
            i = end;
        } else if (c == '$' && i + 1 < v.size() && is_name_start(v[i + 1])) {
            size_t end = i + 1;
            while (end < v.size() && is_name_char(v[end])) ++end;
            out += value_of(std::string(v.substr(i + 1, end - i - 1)));
            i = end - 1;
        } else {
            out += c;
        }
    }
    return out;
}

std::string unescape_double(std::string_view v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 == v.size()) {
            out += v[i];
            continue;
        }
        char n = v[++i];
        switch (n) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '$': out += "\\$"; break;  // kept for expand(), which makes it a literal "$"
            default: out += '\\'; out += n;
        }
    }
    return out;
}

}  // namespace

std::vector<Entry> parse(std::string_view text, const char* (*lookup)(const char*)) {
    std::vector<Entry> entries;
    size_t i = 0;
    const size_t n = text.size();
    auto skip_line = [&] {
        while (i < n && text[i] != '\n') ++i;
    };
    auto skip_blanks = [&] {
        while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    };
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= n) break;
        if (text[i] == '#') {
            skip_line();
            continue;
        }
        if (text.substr(i, 7) == "export " || text.substr(i, 7) == "export\t") {
            i += 7;
            skip_blanks();
        }
        size_t key_start = i;
        while (i < n && is_key_char(text[i])) ++i;
        std::string key(text.substr(key_start, i - key_start));
        skip_blanks();
        if (key.empty() || i >= n || text[i] != '=') {
            skip_line();
            continue;
        }
        ++i;  // '='
        skip_blanks();
        std::string value;
        char q = i < n ? text[i] : '\0';
        if (q == '"' || q == '\'' || q == '`') {
            size_t j = i + 1;
            while (j < n && text[j] != q) j += (q == '"' && text[j] == '\\' && j + 1 < n) ? 2 : 1;
            if (j < n) {  // closed: may span lines
                std::string_view raw = text.substr(i + 1, j - i - 1);
                if (q == '"') value = expand(unescape_double(raw), entries, lookup);
                else value = std::string(raw);
                i = j + 1;
                skip_line();  // anything after the closing quote (a comment)
                entries.push_back({key, value});
                continue;
            }
            // No closing quote: the rest of the line, quote included, like dotenv.
        }
        size_t start = i;
        skip_line();
        std::string_view raw = text.substr(start, i - start);
        for (size_t k = 0; k < raw.size(); ++k) {  // " # comment" ends an unquoted value
            if (raw[k] == '#' && k > 0 && (raw[k - 1] == ' ' || raw[k - 1] == '\t')) {
                raw = raw.substr(0, k);
                break;
            }
        }
        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
        entries.push_back({key, expand(raw, entries, lookup)});
    }
    return entries;
}

bool load(const std::vector<std::string>& explicit_files, bool auto_load, std::string& error) {
    auto env_lookup = [](const char* name) -> const char* { return std::getenv(name); };
    std::vector<std::string> files;  // highest priority first
    if (!explicit_files.empty()) {
        files.assign(explicit_files.rbegin(), explicit_files.rend());
        for (auto& f : files) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(f, ec)) {
                error = f + ": not found";
                return false;
            }
        }
    } else if (auto_load) {
        const char* node_env = std::getenv("NODE_ENV");
        std::string mode = node_env && *node_env ? node_env : "";
        if (mode != "test") files.push_back(".env.local");
        if (!mode.empty() && mode.find('/') == std::string::npos) files.push_back(".env." + mode);
        files.push_back(".env");
    }
    for (auto& f : files) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(f, ec)) continue;
        auto text = read_file(f);
        if (!text) {
            if (!explicit_files.empty()) {
                error = f + ": could not be read";
                return false;
            }
            continue;
        }
        for (auto& e : parse(*text, env_lookup)) setenv(e.key.c_str(), e.value.c_str(), 0);
    }
    return true;
}

}  // namespace rtn::dotenv
