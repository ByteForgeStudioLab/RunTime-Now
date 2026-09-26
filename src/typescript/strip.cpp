// TypeScript -> JavaScript by type erasure.
//
// Pipeline:
//   1. tokenize()   source -> tokens (handles strings, templates, regex, comments)
//   2. pair up brackets  ( [ { ${  ->  match_[open] = close
//   3. walk()       go through the code frame by frame (block, class body,
//                   object literal, parameter list...) and blank out every
//                   piece of type syntax we recognise
//   4. elide_imports()  drop imports that are only used as types
//
// Blanking replaces characters with spaces but keeps '\n', so positions
// in the output are identical to the input.

#include "typescript/strip.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

namespace rtn::ts {

namespace {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

enum class T : uint8_t {
    Ident,
    Number,
    String,
    NoSubstTemplate,  // `abc`
    TemplateHead,     // `abc${
    TemplateMiddle,   // }abc${
    TemplateTail,     // }abc`
    Regex,
    Private,          // #name
    Punct,
    End,
};

struct Token {
    T kind;
    uint32_t start;
    uint32_t end;
    bool nl;  // a line break comes before this token
};

struct StripError {
    size_t pos;
    std::string msg;
};

constexpr size_t kNone = std::numeric_limits<size_t>::max();

bool is_ident_start(unsigned char c) {
    return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80 || c == '\\';
}
bool is_ident_char(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80 || c == '\\';
}

bool in_list(std::string_view s, std::initializer_list<std::string_view> list) {
    for (auto x : list) {
        if (s == x) return true;
    }
    return false;
}

// Keywords after which an expression starts (so `/` is a regex, `{` is an object...).
bool is_operator_keyword(std::string_view s) {
    return in_list(s, {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void",
                       "throw", "case", "do", "else", "yield", "await", "extends", "export",
                       "import", "default", "let", "const", "var", "if", "while", "for",
                       "switch", "try", "catch", "finally", "with", "function", "class"});
}

class Tokenizer {
public:
    explicit Tokenizer(std::string_view s) : s_(s) {}

    std::vector<Token> run() {
        if (s_.starts_with("\xEF\xBB\xBF")) p_ = 3;  // UTF-8 BOM
        if (s_.substr(p_).starts_with("#!")) {       // shebang
            while (p_ < s_.size() && s_[p_] != '\n') ++p_;
        }
        while (true) {
            skip_space_and_comments();
            if (p_ >= s_.size()) {
                push(T::End, p_);
                return std::move(toks_);
            }
            size_t a = p_;
            unsigned char c = s_[p_];
            if (is_ident_start(c)) {
                scan_ident();
                push(T::Ident, a);
            } else if (c == '#' && p_ + 1 < s_.size() && is_ident_start(s_[p_ + 1])) {
                ++p_;
                scan_ident();
                push(T::Private, a);
            } else if (std::isdigit(c) || (c == '.' && p_ + 1 < s_.size() && std::isdigit((unsigned char)s_[p_ + 1]))) {
                scan_number();
                push(T::Number, a);
            } else if (c == '"' || c == '\'') {
                scan_string(c);
                push(T::String, a);
            } else if (c == '`') {
                ++p_;
                push(scan_template(true), a);
            } else if (c == '}' && !braces_.empty() && braces_.back() == 't') {
                braces_.pop_back();
                ++p_;
                push(scan_template(false), a);
            } else if (c == '/') {
                if (regex_allowed()) {
                    scan_regex();
                    push(T::Regex, a);
                } else {
                    p_ += (p_ + 1 < s_.size() && s_[p_ + 1] == '=') ? 2 : 1;
                    push(T::Punct, a);
                }
            } else {
                scan_punct();
                push(T::Punct, a);
            }
        }
    }

private:
    [[noreturn]] void fail(const char* msg) { throw StripError{p_, msg}; }

    void push(T kind, size_t a) {
        toks_.push_back({kind, static_cast<uint32_t>(a), static_cast<uint32_t>(p_), nl_});
        nl_ = false;
    }

    void skip_space_and_comments() {
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (c == '\n') {
                nl_ = true;
                ++p_;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                ++p_;
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/') {
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '*') {
                size_t e = s_.find("*/", p_ + 2);
                if (e == std::string_view::npos) fail("unterminated comment");
                if (s_.substr(p_, e - p_).find('\n') != std::string_view::npos) nl_ = true;
                p_ = e + 2;
            } else if (static_cast<unsigned char>(c) == 0xC2 && p_ + 1 < s_.size() &&
                       static_cast<unsigned char>(s_[p_ + 1]) == 0xA0) {
                p_ += 2;  // non-breaking space
            } else {
                break;
            }
        }
    }

    void scan_ident() {
        while (p_ < s_.size() && is_ident_char(s_[p_])) {
            if (s_[p_] == '\\') p_ += 2; else ++p_;  // \u escapes
        }
    }

    void scan_number() {
        bool hex = s_[p_] == '0' && p_ + 1 < s_.size() && (s_[p_ + 1] == 'x' || s_[p_ + 1] == 'X');
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
                ++p_;
            } else if ((c == '+' || c == '-') && !hex && (s_[p_ - 1] == 'e' || s_[p_ - 1] == 'E')) {
                ++p_;
            } else {
                break;
            }
        }
    }

    void scan_string(char q) {
        ++p_;
        while (p_ < s_.size() && s_[p_] != q) {
            if (s_[p_] == '\\') ++p_;
            else if (s_[p_] == '\n') fail("unterminated string");
            ++p_;
        }
        if (p_ >= s_.size()) fail("unterminated string");
        ++p_;
    }

    // Called right after ` or the } that closes a ${ hole.
    T scan_template(bool from_backtick) {
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (c == '\\') {
                p_ += 2;
            } else if (c == '`') {
                ++p_;
                return from_backtick ? T::NoSubstTemplate : T::TemplateTail;
            } else if (c == '$' && p_ + 1 < s_.size() && s_[p_ + 1] == '{') {
                p_ += 2;
                braces_.push_back('t');
                return from_backtick ? T::TemplateHead : T::TemplateMiddle;
            } else {
                ++p_;
            }
        }
        fail("unterminated template literal");
    }

    bool regex_allowed() const {
        if (toks_.empty()) return true;
        const Token& t = toks_.back();
        std::string_view x = s_.substr(t.start, t.end - t.start);
        switch (t.kind) {
            case T::Ident: return is_operator_keyword(x);
            case T::Punct: return !in_list(x, {")", "]", "}", "++", "--"});
            case T::TemplateHead:
            case T::TemplateMiddle: return true;
            default: return false;
        }
    }

    void scan_regex() {
        ++p_;
        bool in_class = false;
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (c == '\\') {
                p_ += 2;
                continue;
            }
            if (c == '\n') fail("unterminated regular expression");
            if (c == '[') in_class = true;
            else if (c == ']') in_class = false;
            else if (c == '/' && !in_class) break;
            ++p_;
        }
        if (p_ >= s_.size()) fail("unterminated regular expression");
        ++p_;
        while (p_ < s_.size() && is_ident_char(s_[p_])) ++p_;  // flags
    }

    void scan_punct() {
        // '<' and '>' are always single tokens: that makes generics like
        // Map<string, Array<number>> easy to balance. We never need to
        // understand shift/compare operators, we only copy them.
        static constexpr std::string_view kMulti[] = {
            "...", "===", "!==", "**=", "&&=", "||=", "?\?=", "=>", "==", "!=", "**",
            "&&", "||", "??", "++", "--", "+=", "-=", "*=", "%=", "&=", "|=", "^=",
        };
        std::string_view rest = s_.substr(p_);
        if (rest.starts_with("?.") && !(rest.size() > 2 && std::isdigit((unsigned char)rest[2]))) {
            p_ += 2;
            return;
        }
        for (auto m : kMulti) {
            if (rest.starts_with(m)) {
                p_ += m.size();
                return;
            }
        }
        char c = s_[p_++];
        if (c == '{') braces_.push_back('b');
        else if (c == '}' && !braces_.empty()) braces_.pop_back();
    }

    std::string_view s_;
    size_t p_ = 0;
    bool nl_ = false;
    std::vector<char> braces_;  // 'b' = {  't' = ${ in a template
    std::vector<Token> toks_;
};

// ---------------------------------------------------------------------------
// Stripper
// ---------------------------------------------------------------------------

class Stripper {
public:
    explicit Stripper(std::string_view src) : src_(src), out_(src) {}

    std::string run() {
        tok_ = Tokenizer(src_).run();
        pair_brackets();
        State top{Frame::Statements};
        walk(0, tok_.size() - 1, top);
        elide_imports();
        return apply_inserts();
    }

private:
    enum class Frame { Statements, Class, Object, Paren, Params, Bracket, Template };

    struct State {
        Frame kind;
        bool var_decl = false;           // inside `let a: T = 1, b: U`
        int ternary = 0;                 // open `?` waiting for their `:`
        bool colon_was_ternary = false;
        bool member_start = true;        // Class / Object / Params: at the start of an item
        std::vector<std::string>* param_props = nullptr;  // constructor(private x)
        size_t class_open = 0;                            // Class: index of the body's '{'
    };

    struct Binding {
        std::string local;     // name used in this module
        std::string rendered;  // text to emit: "a", "a as b", "* as ns"
        bool type_only;
        enum Kind { Default, Namespace, Named } kind;
    };
    struct ImportExport {
        bool is_import;
        size_t start, end;     // token range of the whole statement
        std::vector<Binding> bindings;
        std::string from;      // "from './x'" (may be empty for local export lists)
    };

    // ----- token helpers -----
    const Token& tk(size_t i) const { return tok_[std::min(i, tok_.size() - 1)]; }
    std::string_view text(size_t i) const {
        const Token& t = tk(i);
        return src_.substr(t.start, t.end - t.start);
    }
    bool is(size_t i, std::string_view p) const { return tk(i).kind == T::Punct && text(i) == p; }
    bool word(size_t i, std::string_view w) const { return tk(i).kind == T::Ident && text(i) == w; }
    bool ident(size_t i) const { return tk(i).kind == T::Ident; }
    bool opener(size_t i) const {
        return tk(i).kind == T::TemplateHead || is(i, "(") || is(i, "[") || is(i, "{");
    }
    bool after_dot(size_t i) const { return i > 0 && (is(i - 1, ".") || is(i - 1, "?.")); }

    [[noreturn]] void fail(size_t i, const std::string& msg) const { throw StripError{tk(i).start, msg}; }

    // Source text of tokens [a, b) on one line (comments/newlines become one space).
    std::string join(size_t a, size_t b) const {
        std::string s;
        for (size_t k = a; k < b; ++k) {
            if (k > a && tk(k).start > tk(k - 1).end) s += ' ';
            s += text(k);
        }
        return s;
    }

    void pair_brackets() {
        match_.assign(tok_.size(), kNone);
        std::vector<size_t> stack;
        for (size_t i = 0; i < tok_.size(); ++i) {
            const Token& t = tok_[i];
            if (opener(i)) {
                stack.push_back(i);
                continue;
            }
            char want = 0;
            if (t.kind == T::TemplateTail || t.kind == T::TemplateMiddle) want = '`';
            else if (is(i, ")")) want = '(';
            else if (is(i, "]")) want = '[';
            else if (is(i, "}")) want = '{';
            if (!want) continue;

            if (stack.empty()) fail(i, "unexpected '" + std::string(text(i).substr(0, 1)) + "'");
            size_t o = stack.back();
            char got = tok_[o].kind == T::TemplateHead ? '`' : text(o)[0];
            if (got != want) fail(i, "mismatched brackets");
            if (t.kind == T::TemplateMiddle) continue;  // stays inside the same template
            match_[o] = i;
            stack.pop_back();
        }
        if (!stack.empty()) fail(stack.back(), "unclosed bracket");
    }

    // ----- output editing -----
    void blank(size_t a, size_t b) {  // token range [a, b)
        if (a >= b) return;
        for (size_t p = tk(a).start; p < tk(b - 1).end; ++p) {
            if (out_[p] != '\n' && out_[p] != '\r') out_[p] = ' ';
        }
    }
    // Removes a whole statement. The leading ';' protects against ASI surprises:
    //   let a = b      let a = b
    //   type T = X  => ;
    //   (f)()          (f)()      <- would otherwise become b(f)()
    void blank_statement(size_t a, size_t b) {
        blank(a, b);
        if (a < b) out_[tk(a).start] = ';';
    }
    bool blanked(size_t i) const { return out_[tk(i).start] != src_[tk(i).start]; }
    void insert(size_t pos, std::string s) { inserts_.push_back({pos, std::move(s)}); }

    std::string apply_inserts() {
        std::stable_sort(inserts_.begin(), inserts_.end(),
                         [](auto& a, auto& b) { return a.first < b.first; });
        std::string r;
        r.reserve(out_.size() + 256);
        size_t p = 0;
        for (auto& [pos, s] : inserts_) {
            r.append(out_, p, pos - p);
            r += s;
            p = pos;
        }
        r.append(out_, p, std::string::npos);
        return r;
    }

    // ----- expression classification -----
    bool is_expr_end(size_t i) const {
        if (i == kNone || i >= tok_.size()) return false;
        switch (tok_[i].kind) {
            case T::Number: case T::String: case T::NoSubstTemplate:
            case T::TemplateTail: case T::Regex: case T::Private:
                return true;
            case T::Punct: return is(i, ")") || is(i, "]") || is(i, "}");
            case T::Ident: return after_dot(i) || !is_operator_keyword(text(i));
            default: return false;
        }
    }

    bool at_stmt_start(size_t i) const {
        return i == 0 || tk(i).nl || is(i - 1, ";") || is(i - 1, "{") || is(i - 1, "}");
    }

    // ----- type skipping (returns index after the type, or 0 on failure) -----

    // <...> with balanced nesting; used where we know it's a type list.
    size_t skip_angle(size_t i) const {
        int depth = 0;
        for (size_t k = i; k < tok_.size() - 1; ++k) {
            if (is(k, "<")) {
                ++depth;
            } else if (is(k, ">")) {
                if (--depth == 0) return k + 1;
            } else if (opener(k)) {
                k = match_[k];
            } else if (is(k, ";") || is(k, ")") || is(k, "]") || is(k, "}")) {
                return 0;
            }
        }
        return 0;
    }

    // <A, B<C>> parsed as real types: used in expressions where `<` may be a comparison.
    size_t skip_type_args(size_t i) const {
        if (is(i + 1, "<") && tk(i + 1).start == tk(i).end) return 0;  // a << b
        size_t k = i + 1;
        while (true) {
            k = skip_type(k);
            if (!k) return 0;
            if (is(k, ",")) {
                ++k;
                continue;
            }
            return is(k, ">") ? k + 1 : 0;
        }
    }

    size_t skip_type(size_t i) const {
        if (i >= tok_.size() - 1) return 0;
        if (is(i, "|") || is(i, "&")) ++i;
        size_t k = skip_union(i);
        if (!k) return 0;
        if (word(k, "extends") && !tk(k).nl) {  // conditional type: A extends B ? X : Y
            size_t c = skip_union(k + 1);
            if (!c || !is(c, "?")) return k;
            size_t t = skip_type(c + 1);
            if (!t || !is(t, ":")) return 0;
            return skip_type(t + 1);
        }
        return k;
    }

    size_t skip_union(size_t i) const {
        size_t k = skip_postfix(i);
        while (k && (is(k, "|") || is(k, "&"))) k = skip_postfix(k + 1);
        return k;
    }

    size_t skip_postfix(size_t i) const {
        size_t k = skip_primary(i);
        while (k && is(k, "[") && !tk(k).nl) k = match_[k] + 1;  // T[]  T[K]
        return k;
    }

    size_t skip_primary(size_t i) const {
        const Token& t = tk(i);
        switch (t.kind) {
            case T::String: case T::Number: case T::NoSubstTemplate:
                return i + 1;
            case T::TemplateHead:
                return match_[i] + 1;
            case T::Punct: {
                if (is(i, "(")) {  // (A | B)  or  (x: A) => B
                    size_t j = match_[i];
                    return is(j + 1, "=>") ? skip_type(j + 2) : j + 1;
                }
                if (is(i, "<")) {  // <T>(x: T) => T
                    size_t j = skip_angle(i);
                    if (!j || !is(j, "(")) return 0;
                    j = match_[j] + 1;
                    return is(j, "=>") ? skip_type(j + 1) : 0;
                }
                if (is(i, "{") || is(i, "[")) return match_[i] + 1;
                if (is(i, "-") && tk(i + 1).kind == T::Number) return i + 2;
                return 0;
            }
            case T::Ident:
                break;
            default:
                return 0;
        }

        std::string_view x = text(i);
        if (x == "new" || (x == "abstract" && word(i + 1, "new"))) {
            size_t j = x == "new" ? i + 1 : i + 2;
            if (is(j, "<")) j = skip_angle(j);
            if (!j || !is(j, "(")) return 0;
            j = match_[j] + 1;
            return is(j, "=>") ? skip_type(j + 1) : 0;
        }
        if (x == "typeof") {
            size_t j = i + 1;
            if (word(j, "import") && is(j + 1, "(")) {
                j = match_[j + 1] + 1;
            } else if (ident(j)) {
                ++j;
            } else {
                return 0;
            }
            while (is(j, ".") && (ident(j + 1) || tk(j + 1).kind == T::Private)) j += 2;
            if (is(j, "<") && !tk(j).nl) {
                if (size_t k = skip_type_args(j)) j = k;
            }
            return j;
        }
        if (in_list(x, {"keyof", "unique", "readonly"}) && !is(i + 1, ",") && !is(i + 1, ">")) {
            return skip_postfix(i + 1);
        }
        if (x == "infer" && ident(i + 1)) return i + 2;
        if (x == "asserts" && ident(i + 1) && !tk(i + 1).nl) {
            size_t j = i + 2;
            return word(j, "is") ? skip_type(j + 1) : j;
        }
        if (word(i + 1, "is") && !tk(i + 1).nl) return skip_type(i + 2);  // x is string
        if (in_list(x, {"function", "class", "return", "if", "else", "for", "while", "do", "switch",
                        "case", "break", "continue", "var", "let", "const", "export", "import", "in",
                        "instanceof", "delete", "throw", "try", "catch", "finally", "with", "yield",
                        "await", "extends", "implements", "as", "satisfies", "enum", "interface"})) {
            return 0;
        }
        size_t j = i + 1;
        while (is(j, ".") && ident(j + 1)) j += 2;  // A.B.C
        if (is(j, "<") && !tk(j).nl) {
            if (size_t k = skip_type_args(j)) j = k;
        }
        return j;
    }

    // ----- the walker -----

    void walk(size_t i, size_t end, State& st) {
        while (i < end) {
            size_t next;
            if (tk(i).kind == T::TemplateMiddle) {
                st.var_decl = false;
                st.ternary = 0;
                next = i + 1;
            } else if (st.kind == Frame::Class) {
                if (!st.member_start && tk(i).nl && is_expr_end(i - 1) && can_start_member(i)) {
                    st.member_start = true;  // ASI between class fields
                }
                next = st.member_start ? class_member(i, end, st) : expression_token(i, st);
            } else if (st.kind == Frame::Object && st.member_start) {
                next = object_member(i, st);
            } else if (st.kind == Frame::Params && st.member_start) {
                next = param(i, st);
            } else {
                next = i;
                if (st.kind == Frame::Statements) next = statement_level(i, st);
                if (next == i) next = expression_token(i, st);
            }
            if (next > end) fail(end, "unexpected end of block");
            i = next;
        }
    }

    void walk_group(size_t open, Frame kind, std::vector<std::string>* props = nullptr) {
        State st{kind};
        st.param_props = props;
        walk(open + 1, match_[open], st);
    }

    size_t expression_token(size_t i, State& st) {
        const Token& t = tk(i);
        if (t.kind == T::TemplateHead) {
            walk_group(i, Frame::Template);
            return match_[i] + 1;
        }
        if (t.kind == T::Punct) {
            std::string_view x = text(i);
            if (x == "(") return open_paren(i, st);
            if (x == "[") {
                walk_group(i, Frame::Bracket);
                return match_[i] + 1;
            }
            if (x == "{") {
                walk_group(i, brace_kind(i, st));
                return match_[i] + 1;
            }
            if (x == "<") return angle(i);
            if (x == "!") {  // non-null assertion: value!
                if (!t.nl && is_expr_end(i - 1) && !is(i - 1, "}")) blank(i, i + 1);
                return i + 1;
            }
            if (x == "?") {
                ++st.ternary;
                return i + 1;
            }
            if (x == ":") {
                st.colon_was_ternary = st.ternary > 0;
                if (st.ternary > 0) --st.ternary;
                return i + 1;
            }
            if (x == ",") {
                if (st.var_decl) return binding(i + 1, st);
                if (st.kind == Frame::Object || st.kind == Frame::Params) st.member_start = true;
                return i + 1;
            }
            if (x == ";") {
                st.var_decl = false;
                st.ternary = 0;
                if (st.kind == Frame::Class) st.member_start = true;
                return i + 1;
            }
            if (x == "@") fail(i, "decorators are not supported yet");
            return i + 1;
        }
        if (t.kind != T::Ident || after_dot(i)) return i + 1;

        std::string_view x = text(i);
        if (x == "const" || x == "var" || (x == "let" && (ident(i + 1) || is(i + 1, "{") || is(i + 1, "[")))) {
            st.var_decl = true;
            return binding(i + 1, st);
        }
        if (x == "function") return function_(i);
        if (x == "class") return class_(i);
        if (x == "catch" && is(i + 1, "(")) {  // catch (e: unknown)
            walk_group(i + 1, Frame::Params);
            return match_[i + 1] + 1;
        }
        if ((x == "as" || x == "satisfies") && !t.nl && is_expr_end(i - 1)) {
            if (x == "as" && word(i + 1, "const")) {  // as const
                blank(i, i + 2);
                return i + 2;
            }
            if (size_t k = skip_type(i + 1)) {
                blank(i, k);
                return k;
            }
        }
        return i + 1;
    }

    Frame brace_kind(size_t i, const State& st) const {
        if (i == 0) return Frame::Statements;
        bool stmts = st.kind == Frame::Statements;
        size_t p = i - 1;
        const Token& t = tk(p);
        if (t.kind == T::Punct) {
            std::string_view x = text(p);
            if (x == "=>" || x == ")") return Frame::Statements;
            if (x == ";" || x == "{" || x == "}") return stmts ? Frame::Statements : Frame::Object;
            if (x == ":") return (stmts && !st.colon_was_ternary) ? Frame::Statements : Frame::Object;
            return Frame::Object;
        }
        if (t.kind == T::Ident) {
            std::string_view x = text(p);
            if (in_list(x, {"else", "try", "finally", "do"})) return Frame::Statements;
            if (is_operator_keyword(x)) return Frame::Object;
        }
        return stmts ? Frame::Statements : Frame::Object;
    }

    // `(`: a parenthesised expression, a call, or arrow function parameters.
    size_t open_paren(size_t i, State& st) {
        size_t j = match_[i];
        size_t ret_end = 0;
        bool arrow = is(j + 1, "=>");
        if (!arrow && is(j + 1, ":") && (st.ternary == 0 || params_look_typed(i))) {
            size_t k = skip_type(j + 2);  // (x): Type => ...
            if (k && is(k, "=>")) {
                arrow = true;
                ret_end = k;
            }
        }
        walk_group(i, arrow ? Frame::Params : Frame::Paren);
        if (ret_end) {
            blank(j + 1, ret_end);
            // JS forbids a line break between ')' and '=>', so when the return
            // type spanned several lines, move the ')' down next to the arrow.
            if (src_.substr(tk(j).end, tk(ret_end).start - tk(j).end).find('\n') != std::string_view::npos) {
                blank(j, j + 1);
                size_t before = tk(ret_end).start - 1;
                if (out_[before] == ' ') out_[before] = ')';
                else insert(tk(ret_end).start, ")");
            }
            return ret_end;
        }
        return j + 1;
    }

    // Inside a ternary, `(a: T): U => x` is still an arrow if the parens contain `:`.
    bool params_look_typed(size_t open) const {
        bool colon = false;
        for (size_t k = open + 1; k < match_[open]; ++k) {
            if (opener(k)) k = match_[k];
            else if (is(k, "?")) return false;
            else if (is(k, ":")) colon = true;
        }
        return colon;
    }

    // `<` in an expression: generic arrow `<T>(x: T) => x`, or call type args `f<T>()`.
    size_t angle(size_t i) {
        size_t p = i - 1;
        bool expr_start = i == 0 ||
                          (tk(p).kind == T::Punct && !in_list(text(p), {")", "]", "}", "<", ">"})) ||
                          (tk(p).kind == T::Ident && (is_operator_keyword(text(p)) || text(p) == "async"));
        if (expr_start) {
            // <T>(x: T) => x   or the old-style assertion <T>value
            size_t k = skip_angle(i);
            bool value_follows = k && (tk(k).kind != T::Punct ||
                                       in_list(text(k), {"(", "[", "{", "!", "-", "+", "~", "...", "<"}));
            if (value_follows) {
                blank(i, k);
                return k;
            }
            return i + 1;
        }
        if (ident(p) && !is_operator_keyword(text(p))) {
            size_t k = skip_type_args(i);
            if (k && (is(k, "(") || tk(k).kind == T::TemplateHead ||
                      tk(k).kind == T::NoSubstTemplate || preceded_by_new(p))) {
                blank(i, k);
                return k;
            }
        }
        return i + 1;
    }

    bool preceded_by_new(size_t p) const {
        while (p >= 2 && is(p - 1, ".") && ident(p - 2)) p -= 2;
        return p >= 1 && word(p - 1, "new");
    }

    // After let/const/var or a comma in a declaration list: `x!: T`, `{a, b}: T`
    size_t binding(size_t i, State& st) {
        (void)st;
        if (ident(i) && !word(i, "of") && !word(i, "in")) {
            ++i;
        } else if (is(i, "{")) {
            walk_group(i, Frame::Object);
            i = match_[i] + 1;
        } else if (is(i, "[")) {
            walk_group(i, Frame::Bracket);
            i = match_[i] + 1;
        } else {
            return i;
        }
        if (is(i, "!") && is(i + 1, ":")) {
            blank(i, i + 1);
            ++i;
        }
        if (is(i, ":")) {
            size_t k = skip_type(i + 1);
            if (!k) fail(i + 1, "could not parse type annotation");
            blank(i, k);
            i = k;
        }
        return i;
    }

    // One parameter: `private readonly x?: T = 1`, `this: Foo`, `...rest: T[]`
    size_t param(size_t i, State& st) {
        size_t start = i;
        if (is(i, "@")) fail(i, "decorators are not supported yet");
        if (word(i, "this") && is(i + 1, ":")) {
            size_t k = skip_type(i + 2);
            if (!k) fail(i + 2, "could not parse type annotation");
            if (is(k, ",")) ++k;
            blank(start, k);
            return k;
        }
        bool has_mod = false;
        while (ident(i) && in_list(text(i), {"public", "private", "protected", "readonly", "override"}) &&
               (ident(i + 1) || is(i + 1, "{") || is(i + 1, "["))) {
            blank(i, i + 1);
            has_mod = true;
            ++i;
        }
        if (is(i, "...")) ++i;
        std::string name;
        if (ident(i)) {
            name = text(i);
            ++i;
        } else if (is(i, "{")) {
            walk_group(i, Frame::Object);
            i = match_[i] + 1;
        } else if (is(i, "[")) {
            walk_group(i, Frame::Bracket);
            i = match_[i] + 1;
        } else {
            st.member_start = false;
            return i;
        }
        if (has_mod) {
            if (!st.param_props || name.empty()) fail(start, "parameter properties are only allowed in constructors");
            st.param_props->push_back(name);
        }
        if (is(i, "?")) {
            blank(i, i + 1);
            ++i;
        }
        if (is(i, ":")) {
            size_t k = skip_type(i + 1);
            if (!k) fail(i + 1, "could not parse type annotation");
            blank(i, k);
            i = k;
        }
        st.member_start = false;  // a default value may follow; ',' starts the next one
        return i;
    }

    // function name<T>(params): Ret { body }   — or an overload without a body.
    size_t function_(size_t i) {
        size_t j = i + 1;
        if (is(j, "*")) ++j;
        if (ident(j)) ++j;
        if (is(j, "<")) {
            size_t k = skip_angle(j);
            if (!k) fail(j, "could not parse type parameters");
            blank(j, k);
            j = k;
        }
        if (!is(j, "(")) fail(j, "expected '(' after function");
        size_t close = match_[j];
        walk_group(j, Frame::Params);
        j = close + 1;
        j = return_type(j);
        if (is(j, "{")) {
            walk_group(j, Frame::Statements);
            return match_[j] + 1;
        }
        // Overload signature: `function f(a: string): void;`
        size_t s = i;
        if (s > 0 && word(s - 1, "async")) --s;
        if (s > 0 && word(s - 1, "default")) --s;
        if (s > 0 && word(s - 1, "export")) --s;
        size_t e = is(j, ";") ? j + 1 : j;
        blank_statement(s, e);
        return e;
    }

    size_t return_type(size_t j) {
        if (!is(j, ":")) return j;
        size_t k = skip_type(j + 1);
        if (!k) fail(j + 1, "could not parse return type");
        blank(j, k);
        return k;
    }

    // class Name<T> extends Base<T> implements I, J { ... }
    size_t class_(size_t i) {
        size_t j = i + 1;
        if (ident(j) && !word(j, "extends") && !word(j, "implements")) ++j;
        if (is(j, "<")) {
            size_t k = skip_angle(j);
            if (!k) fail(j, "could not parse type parameters");
            blank(j, k);
            j = k;
        }
        if (word(j, "extends")) {
            ++j;
            while (j < tok_.size() - 1 && !is(j, "{") && !word(j, "implements")) {
                if (is(j, "<")) {
                    size_t k = skip_angle(j);
                    if (!k) fail(j, "could not parse type arguments");
                    blank(j, k);
                    j = k;
                } else if (opener(j)) {
                    walk_group(j, Frame::Paren);
                    j = match_[j] + 1;
                } else {
                    ++j;
                }
            }
        }
        if (word(j, "implements")) {
            size_t k = j;
            while (k < tok_.size() - 1 && !is(k, "{")) {
                if (is(k, "<")) k = std::max(skip_angle(k), k + 1);
                else if (opener(k)) k = match_[k] + 1;
                else ++k;
            }
            blank(j, k);
            j = k;
        }
        if (!is(j, "{")) fail(j, "expected class body");
        State body{Frame::Class};
        body.class_open = j;
        walk(j + 1, match_[j], body);
        return match_[j] + 1;
    }

    bool can_start_member(size_t i) const {
        T k = tk(i).kind;
        return k == T::Ident || k == T::String || k == T::Number || k == T::Private ||
               is(i, "*") || is(i, "@");
    }

    // A modifier word is really a modifier only if a member name follows it.
    bool modifier_applies(size_t next) const {
        T k = tk(next).kind;
        return k == T::Ident || k == T::String || k == T::Number || k == T::Private ||
               is(next, "[") || is(next, "*") || is(next, "{");
    }

    size_t class_member(size_t i, size_t end, State& st) {
        size_t start = i;
        if (is(i, ";")) return i + 1;
        if (is(i, "@")) fail(i, "decorators are not supported yet");

        bool is_abstract = false, is_declare = false;
        while (i < end && ident(i) && modifier_applies(i + 1) &&
               in_list(text(i), {"public", "private", "protected", "readonly", "abstract", "override",
                                 "declare", "static", "accessor", "async", "get", "set"})) {
            std::string_view m = text(i);
            if (m == "static" && is(i + 1, "{")) {  // static { ... }
                walk_group(i + 1, Frame::Statements);
                st.member_start = true;
                return match_[i + 1] + 1;
            }
            if (m == "async" && tk(i + 1).nl) break;
            if (in_list(m, {"public", "private", "protected", "readonly", "abstract", "override", "declare"})) {
                blank(i, i + 1);
                is_abstract |= m == "abstract";
                is_declare |= m == "declare";
            }
            ++i;
        }
        if (is(i, "*")) ++i;

        size_t name_idx = i;
        if (is(i, "[")) {
            if (ident(i + 1) && is(i + 2, ":")) {  // index signature: [key: string]: T;
                size_t k = match_[i] + 1;
                if (is(k, ":")) k = skip_type(k + 1);
                if (!k) fail(i, "could not parse index signature");
                if (is(k, ";")) ++k;
                blank_statement(start, k);
                st.member_start = true;
                return k;
            }
            walk_group(i, Frame::Bracket);  // computed name
            i = match_[i] + 1;
        } else if (ident(i) || tk(i).kind == T::String || tk(i).kind == T::Number || tk(i).kind == T::Private) {
            ++i;
        } else {
            fail(i, "unexpected token in class body");
        }

        if (is(i, "?") || is(i, "!")) {  // optional / definite
            blank(i, i + 1);
            ++i;
        }

        if (is(i, "(") || is(i, "<")) {  // method
            if (is(i, "<")) {
                size_t k = skip_angle(i);
                if (!k) fail(i, "could not parse type parameters");
                blank(i, k);
                i = k;
            }
            if (!is(i, "(")) fail(i, "expected '('");
            std::vector<std::string> props;
            bool ctor = text(name_idx) == "constructor";
            size_t close = match_[i];
            walk_group(i, Frame::Params, ctor ? &props : nullptr);
            i = return_type(close + 1);
            if (is(i, "{")) {
                if (!props.empty()) {
                    insert_param_props(i, props);
                    // Like TS with useDefineForClassFields: declare them as the first fields.
                    std::string fields;
                    for (const auto& p : props) fields += " " + p + ";";
                    insert(tk(st.class_open).end, fields);
                }
                walk_group(i, Frame::Statements);
                st.member_start = true;
                return match_[i] + 1;
            }
            size_t e = is(i, ";") ? i + 1 : i;  // abstract method or overload
            blank_statement(start, e);
            st.member_start = true;
            return e;
        }

        if (is(i, ":")) {  // field type
            size_t k = skip_type(i + 1);
            if (!k) fail(i + 1, "could not parse type annotation");
            blank(i, k);
            i = k;
        }
        if (is_declare || is_abstract) {  // no runtime field at all
            size_t e = is(i, ";") ? i + 1 : i;
            blank_statement(start, e);
            st.member_start = true;
            return e;
        }
        if (is(i, "=")) {  // initializer: an expression until ';' or a new line
            st.member_start = false;
            st.ternary = 0;
            return i + 1;
        }
        st.member_start = true;
        return i;
    }

    // constructor(private x: number) { ... }  ->  { this.x = x; ... }
    // (after super(...) when the class extends another one)
    void insert_param_props(size_t brace, const std::vector<std::string>& props) {
        std::string code;
        for (const auto& p : props) code += " this." + p + " = " + p + ";";
        size_t pos = tk(brace).end;
        for (size_t k = brace + 1; k < match_[brace]; ++k) {
            if (word(k, "super") && is(k + 1, "(")) {
                size_t e = match_[k + 1];
                if (is(e + 1, ";")) ++e;
                else code = ";" + code;  // super()  <- no semicolon
                pos = tk(e).end;
                break;
            }
            if (opener(k)) k = match_[k];
        }
        insert(pos, code);
    }

    size_t object_member(size_t i, State& st) {
        st.member_start = false;
        if (is(i, "...")) return i + 1;
        size_t j = i;
        while (ident(j) && in_list(text(j), {"async", "get", "set"}) &&
               (ident(j + 1) || tk(j + 1).kind == T::String || tk(j + 1).kind == T::Number ||
                is(j + 1, "[") || is(j + 1, "*"))) {
            ++j;
        }
        if (is(j, "*")) ++j;
        if (is(j, "[")) {
            walk_group(j, Frame::Bracket);
            j = match_[j] + 1;
        } else if (ident(j) || tk(j).kind == T::String || tk(j).kind == T::Number) {
            ++j;
        } else {
            return i;  // let the expression code handle it
        }
        if (is(j, "(") || is(j, "<")) {  // method(a: T): R { }
            if (is(j, "<")) {
                size_t k = skip_angle(j);
                if (!k) fail(j, "could not parse type parameters");
                blank(j, k);
                j = k;
            }
            if (!is(j, "(")) return j;
            size_t close = match_[j];
            walk_group(j, Frame::Params);
            j = return_type(close + 1);
            if (is(j, "{")) {
                walk_group(j, Frame::Statements);
                j = match_[j] + 1;
            }
        }
        return j;
    }

    // ----- statement-level TypeScript constructs -----

    size_t statement_level(size_t i, State& st) {
        (void)st;
        if (!ident(i) || after_dot(i)) return i;
        std::string_view x = text(i);
        if (x == "import") return import_(i);
        if (x == "export") return export_(i);
        if (!at_stmt_start(i)) return i;
        return ts_declaration(i, i);
    }

    // interface / type / declare / enum / abstract class / namespace.
    // `s` is where the statement starts (the `export` keyword, if any).
    size_t ts_declaration(size_t i, size_t s) {
        std::string_view x = text(i);
        bool same_line_ident = ident(i + 1) && !tk(i + 1).nl;

        if (x == "interface" && same_line_ident) {
            size_t k = i + 2;
            while (k < tok_.size() - 1 && !is(k, "{")) {
                if (is(k, "<")) k = std::max(skip_angle(k), k + 1);
                else if (opener(k)) k = match_[k] + 1;
                else ++k;
            }
            if (!is(k, "{")) fail(i, "expected interface body");
            type_names_.insert(std::string(text(i + 1)));
            size_t e = match_[k] + 1;
            blank_statement(s, e);
            return e;
        }
        if (x == "type" && same_line_ident && (is(i + 2, "=") || is(i + 2, "<"))) {
            size_t e = skip_type_alias(i);
            type_names_.insert(std::string(text(i + 1)));
            blank_statement(s, e);
            return e;
        }
        if (x == "declare" && same_line_ident) {
            size_t e = skip_declare(i + 1);
            blank_statement(s, e);
            return e;
        }
        if (x == "enum" && same_line_ident) return enum_(i, i);
        if (x == "const" && word(i + 1, "enum") && ident(i + 2)) return enum_(i, i + 1);
        if (x == "abstract" && word(i + 1, "class") && !tk(i + 1).nl) {
            blank(i, i + 1);
            return class_(i + 1);
        }
        if ((x == "namespace" || x == "module") && same_line_ident && (is(i + 2, "{") || is(i + 2, "."))) {
            size_t k = i + 2;
            while (is(k, ".") && ident(k + 1)) k += 2;  // namespace A.B.C
            if (is(k, "{") && namespace_is_type_only(k)) {
                blank_statement(s, match_[k] + 1);
                return match_[k] + 1;
            }
            fail(i, "namespaces with runtime code are not supported (use ES modules instead)");
        }
        return i;
    }

    // A namespace that only holds types produces no JavaScript and can be erased.
    bool namespace_is_type_only(size_t open) {
        size_t close = match_[open];
        for (size_t k = open + 1; k < close;) {
            if (is(k, ";")) {
                ++k;
                continue;
            }
            if (word(k, "export")) ++k;
            std::string_view x = text(k);
            if (x == "interface" && ident(k + 1)) {
                while (k < close && !is(k, "{")) k = is(k, "<") ? std::max(skip_angle(k), k + 1) : k + 1;
                if (!is(k, "{")) return false;
                k = match_[k] + 1;
            } else if (x == "type" && ident(k + 1) && (is(k + 2, "=") || is(k + 2, "<"))) {
                k = skip_type_alias(k);
            } else if (x == "declare" && ident(k + 1)) {
                k = skip_declare(k + 1);
            } else if ((x == "namespace" || x == "module") && ident(k + 1)) {
                k += 2;
                while (is(k, ".") && ident(k + 1)) k += 2;
                if (!is(k, "{") || !namespace_is_type_only(k)) return false;
                k = match_[k] + 1;
            } else {
                return false;
            }
        }
        return true;
    }

    // type Name<T> = ...;
    size_t skip_type_alias(size_t i) {
        size_t k = i + 2;
        if (is(k, "<")) {
            k = skip_angle(k);
            if (!k) fail(i, "could not parse type parameters");
        }
        if (!is(k, "=")) fail(k, "expected '=' in type alias");
        size_t e = skip_type(k + 1);
        if (!e) fail(k + 1, "could not parse type");
        if (is(e, ";")) ++e;
        return e;
    }

    // Everything after `declare` produces no JavaScript.
    size_t skip_declare(size_t j) {
        std::string_view kw = text(j);
        if (kw == "abstract") kw = text(++j);
        auto to_body_end = [&](size_t k) {
            while (k < tok_.size() - 1 && !is(k, "{") && !is(k, ";")) {
                if (is(k, "<")) k = std::max(skip_angle(k), k + 1);
                else if (opener(k)) k = match_[k] + 1;
                else ++k;
            }
            if (is(k, ";")) return k + 1;  // declare module "x";
            if (!is(k, "{")) fail(j, "expected '{'");
            return match_[k] + 1;
        };
        if (in_list(kw, {"global", "module", "namespace", "class", "interface", "enum"})) {
            if (ident(j + 1) && kw != "global") type_names_.insert(std::string(text(j + 1)));
            return to_body_end(j + 1);
        }
        if (kw == "const" && word(j + 1, "enum")) return to_body_end(j + 2);
        if (kw == "type") return skip_type_alias(j);
        if (kw == "function") {
            size_t k = j + 1;
            if (ident(k)) type_names_.insert(std::string(text(k++)));
            if (is(k, "<")) k = skip_angle(k);
            if (!k || !is(k, "(")) fail(j, "expected '('");
            k = match_[k] + 1;
            if (is(k, ":")) k = skip_type(k + 1);
            if (!k) fail(j, "could not parse return type");
            return is(k, ";") ? k + 1 : k;
        }
        if (in_list(kw, {"const", "let", "var"})) {
            size_t k = j + 1;
            while (true) {
                if (ident(k)) type_names_.insert(std::string(text(k++)));
                else if (opener(k)) k = match_[k] + 1;
                if (is(k, ":")) k = skip_type(k + 1);
                if (!k) fail(j, "could not parse type");
                if (!is(k, ",")) break;
                ++k;
            }
            return is(k, ";") ? k + 1 : k;
        }
        fail(j, "unsupported 'declare' form");
    }

    // enum Color { Red, Green = 5, Blue = "b" }
    //   ->  var Color; (function (Color) { ... })(Color || (Color = {}));
    // Everything stays on the enum's first line so line numbers don't move.
    size_t enum_(size_t first, size_t kw) {
        size_t name_idx = kw + 1;
        size_t open = kw + 2;
        if (!ident(name_idx) || !is(open, "{")) fail(kw, "expected enum name and body");
        std::string name(text(name_idx));
        size_t close = match_[open];

        std::string body;
        for (size_t k = open + 1; k < close;) {
            std::string key;
            std::string local;
            if (ident(k)) {
                local = text(k);
                key = "\"" + local + "\"";
            } else if (tk(k).kind == T::String) {
                key = text(k);
            } else {
                fail(k, "expected enum member name");
            }
            ++k;
            std::string value = "__v + 1";
            if (is(k, "=")) {
                size_t a = ++k;
                while (k < close && !is(k, ",")) k = opener(k) ? match_[k] + 1 : k + 1;
                value = "(" + join(a, k) + ")";
            }
            if (is(k, ",")) ++k;
            body += " __v = __s(" + key + ", " + value + ");";
            if (!local.empty() && local != name && !is_operator_keyword(local)) {
                body += " const " + local + " = __v;";  // later members may refer to it
            }
        }
        std::string code = "var " + name + "; (function (" + name + ") { var __v = -1; " +
                           "const __s = (k, v) => { " + name + "[k] = v; if (typeof v === \"number\") " +
                           name + "[v] = k; return v; };" + body + " })(" + name + " || (" + name +
                           " = {}));";
        blank(first, close + 1);
        insert(tk(first).start, code);
        return close + 1;
    }

    // import ... from "x";  (records bindings so unused type imports can be dropped)
    size_t import_(size_t i) {
        size_t j = i + 1;
        if (is(j, "(") || is(j, ".")) return i;  // import("x") / import.meta
        if (tk(j).kind == T::String) {            // import "x";
            size_t e = skip_attributes(j + 1);
            return is(e, ";") ? e + 1 : e;
        }
        bool type_only = word(j, "type") && !word(j + 1, "from") && !is(j + 1, ",");
        if ((ident(j) && is(j + 1, "=")) || (type_only && is(j + 2, "="))) {
            if (!type_only) fail(i, "'import x = require()' is not supported, use ES imports");
            size_t e = j + 3;
            while (e < tok_.size() - 1 && !is(e, ";") && !tk(e).nl) ++e;
            if (is(e, ";")) ++e;
            blank_statement(i, e);
            return e;
        }

        size_t k = j;
        while (k < tok_.size() - 1 && !(word(k, "from") && tk(k + 1).kind == T::String)) {
            k = is(k, "{") ? match_[k] + 1 : k + 1;
        }
        if (!word(k, "from")) fail(i, "expected 'from' in import");
        size_t e = skip_attributes(k + 2);
        std::string from = join(k, e);
        if (is(e, ";")) ++e;

        ImportExport stmt{true, i, e, {}, from};
        for (size_t c = j; c < k;) {
            if (is(c, ",")) {
                ++c;
            } else if (is(c, "*")) {  // * as ns
                stmt.bindings.push_back({std::string(text(c + 2)), "* as " + std::string(text(c + 2)),
                                         false, Binding::Namespace});
                c += 3;
            } else if (is(c, "{")) {
                parse_specifiers(c, stmt.bindings);
                c = match_[c] + 1;
            } else if (!(type_only && c == j)) {
                stmt.bindings.push_back({std::string(text(c)), std::string(text(c)), false, Binding::Default});
                ++c;
            } else {
                ++c;  // the `type` in `import type ...`
            }
        }
        for (auto& b : stmt.bindings) {
            b.type_only |= type_only;
            if (b.type_only) type_names_.insert(b.local);  // so `export { T }` can be dropped later
        }
        if (type_only) {
            blank_statement(i, e);
            return e;
        }
        statements_.push_back(std::move(stmt));
        return e;
    }

    size_t skip_attributes(size_t k) const {  // with { type: "json" }
        if ((word(k, "with") || word(k, "assert")) && is(k + 1, "{")) return match_[k + 1] + 1;
        return k;
    }

    // { a, type B, c as d }
    void parse_specifiers(size_t open, std::vector<Binding>& out) {
        size_t a = open + 1;
        size_t close = match_[open];
        while (a < close) {
            size_t b = a;
            while (b < close && !is(b, ",")) ++b;
            if (b > a) {
                size_t n = b - a;
                bool type_only = word(a, "type") && n >= 2 && !(n == 3 && word(a + 1, "as"));
                size_t first = type_only ? a + 1 : a;
                out.push_back({std::string(text(b - 1)), join(first, b), type_only, Binding::Named});
            }
            a = b + 1;
        }
    }

    size_t export_(size_t i) {
        size_t j = i + 1;
        if (word(j, "type") && (is(j + 1, "{") || is(j + 1, "*"))) {  // export type { A } / export type *
            size_t k = j + 1;
            if (is(k, "{")) {
                k = match_[k] + 1;
            } else {
                ++k;
                if (word(k, "as")) k += 2;
            }
            if (word(k, "from")) k = skip_attributes(k + 2);
            if (is(k, ";")) ++k;
            blank_statement(i, k);
            return k;
        }
        if (word(j, "default")) {
            if (word(j + 1, "interface")) return ts_declaration(j + 1, i);
            if (word(j + 1, "abstract") && word(j + 2, "class")) blank(j + 1, j + 2);
            return j + 1;
        }
        if (is(j, "=")) fail(i, "'export =' is not supported, use 'export default'");
        if (word(j, "import")) fail(i, "'export import' is not supported");
        if (word(j, "as") && word(j + 1, "namespace")) {
            size_t k = j + 3;
            if (is(k, ";")) ++k;
            blank_statement(i, k);
            return k;
        }
        if (is(j, "{")) {  // export { a, type B } [from "x"];
            ImportExport stmt{false, i, 0, {}, ""};
            parse_specifiers(j, stmt.bindings);
            size_t k = match_[j] + 1;
            if (word(k, "from")) {
                size_t e = skip_attributes(k + 2);
                stmt.from = join(k, e);
                k = e;
            }
            if (is(k, ";")) ++k;
            stmt.end = k;
            statements_.push_back(std::move(stmt));
            return k;
        }
        size_t r = ts_declaration(j, i);
        return r != j ? r : j;
    }

    // ----- import elision -----
    // Like tsc: an imported name that is never used as a value (only in types,
    // which we erased) is removed, so we don't try to import a type at runtime.
    void elide_imports() {
        if (statements_.empty()) return;
        std::vector<bool> skip(tok_.size(), false);
        for (const auto& s : statements_) {
            for (size_t k = s.start; k < s.end; ++k) skip[k] = true;
        }
        std::unordered_map<std::string_view, int> uses;
        for (size_t k = 0; k + 1 < tok_.size(); ++k) {
            if (skip[k] || !ident(k) || blanked(k) || after_dot(k)) continue;
            ++uses[text(k)];
        }
        // Names re-exported with `export { x }` also count as uses of an import.
        std::unordered_map<std::string, int> exported;
        for (const auto& s : statements_) {
            if (s.is_import || !s.from.empty()) continue;
            for (const auto& b : s.bindings) {
                if (!b.type_only) ++exported[b.rendered.substr(0, b.rendered.find(' '))];
            }
        }

        for (const auto& s : statements_) {
            std::vector<const Binding*> keep;
            for (const auto& b : s.bindings) {
                if (b.type_only) continue;
                if (s.is_import) {
                    if (uses[b.local] > 0 || exported[b.local] > 0) keep.push_back(&b);
                } else {
                    std::string local = b.rendered.substr(0, b.rendered.find(' '));
                    bool type_decl = s.from.empty() && type_names_.count(local) && uses[local] == 0;
                    if (!type_decl) keep.push_back(&b);
                }
            }
            if (keep.size() == s.bindings.size()) continue;
            if (keep.empty() && !s.bindings.empty()) {
                blank_statement(s.start, s.end);
                continue;
            }
            std::string code = s.is_import ? "import " : "export ";
            std::string named;
            bool first = true;
            for (const Binding* b : keep) {
                if (b->kind == Binding::Named) {
                    named += (named.empty() ? "" : ", ") + b->rendered;
                } else {
                    code += (first ? "" : ", ") + b->rendered;
                    first = false;
                }
            }
            if (!named.empty() || !s.is_import) code += std::string(first ? "" : ", ") + "{ " + named + " }";
            if (!s.from.empty()) code += " " + s.from;
            code += ";";
            blank(s.start, s.end);
            insert(tk(s.start).start, code);
        }
    }

    std::string_view src_;
    std::string out_;
    std::vector<Token> tok_;
    std::vector<size_t> match_;
    std::vector<std::pair<size_t, std::string>> inserts_;
    std::vector<ImportExport> statements_;
    std::set<std::string> type_names_;  // interfaces / type aliases / declares
};

void position_of(std::string_view src, size_t pos, int& line, int& col) {
    line = 1;
    col = 1;
    for (size_t i = 0; i < pos && i < src.size(); ++i) {
        if (src[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
}

}  // namespace

StripResult strip_types(std::string_view source) {
    StripResult r;
    try {
        r.code = Stripper(source).run();
        r.ok = true;
    } catch (const StripError& e) {
        r.error = e.msg;
        position_of(source, e.pos, r.line, r.column);
    }
    return r;
}

}  // namespace rtn::ts
