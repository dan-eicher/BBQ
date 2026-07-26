// ============================================================
// PegRuntime.h — Runtime support for pegc-generated parsers
//
// Provides cursor-based parsing with save/restore for PEG
// backtracking, character/string matching, whitespace+comment
// skipping, and basic error reporting.
//
// Shipped with generated parsers (not generated itself).
// ============================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

namespace peg {

// ── Position mark for backtracking ──────────────────────────

struct Mark {
    const char* pos;
    int line;
    int col;
};

// ── Comment specification ───────────────────────────────────

struct CommentSpec {
    const char* open;
    const char* close;
    bool nested;
    bool structured = false;   // skip a balanced ()-nested, string-aware token group (annotations); language-specific awareness is a backend override
};

// ── Zero-copy span (pointer into input buffer) ──────────────

struct Span {
    const char* ptr = nullptr;
    int len = 0;

    std::string to_string() const { return std::string(ptr, ptr + len); }
};

// ── Span helpers (token payload decoding) ───────────────────

// Decode a string-literal token's captured span into the unescaped
// payload. Span may be the whole quoted form (including surrounding
// double quotes) or just the inner content; surrounding quotes are
// stripped if present. Resolves: \n \r \t \\ \" \' \0 (others pass).
inline std::string unescape_string_lit(Span s) {
    const char* p = s.ptr;
    const char* end = s.ptr + s.len;
    if (s.len >= 2 && *p == '"' && *(end - 1) == '"') { ++p; --end; }
    std::string out;
    out.reserve(static_cast<size_t>(end - p));
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            switch (*p) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '\'': out += '\''; break;
                case '0': out += '\0'; break;
                default: out += *p; break;
            }
        } else {
            out += *p;
        }
        ++p;
    }
    return out;
}

// Decode a char-literal token's captured span into the unescaped char.
// Span may be the whole quoted form ('x' or '\\n') or just the inner
// content; surrounding single quotes are stripped if present.
inline char unescape_char_lit(Span s) {
    const char* p = s.ptr;
    const char* end = s.ptr + s.len;
    if (s.len >= 2 && *p == '\'' && *(end - 1) == '\'') { ++p; --end; }
    if (p >= end) return '\0';
    if (*p == '\\' && p + 1 < end) {
        switch (p[1]) {
            case 'n': return '\n';
            case 'r': return '\r';
            case 't': return '\t';
            // The rest of the standard control escapes. Their absence was not inert: the
            // grammar's char_lit accepts '\\' ANY and the default arm below returns the
            // LETTER, so a grammar writing IGNORE ... + '\f' silently got 'f' — the letter f
            // became whitespace, and the form feed the author asked for was never skipped.
            case 'f': return '\f';
            case 'b': return '\b';
            case 'v': return '\v';
            case 'a': return '\a';
            case '\\': return '\\';
            case '\'': return '\'';
            case '"': return '"';
            case '0': return '\0';
            case 'x': {
                // \xNN — exactly two hex digits. The grammar (char_lit
                // rule in peg.peg) enforces the digit count; here we
                // just decode the captured bytes.
                if (p + 4 > end) return p[1];
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                    return -1;
                };
                int hi = hex(p[2]), lo = hex(p[3]);
                if (hi < 0 || lo < 0) return p[1];
                return static_cast<char>((hi << 4) | lo);
            }
            default: return p[1];
        }
    }
    return *p;
}

// ── Parse error ─────────────────────────────────────────────

struct ParseError {
    int line;
    int col;
    std::string message;
};

// ── PegCursor ───────────────────────────────────────────────
// The main runtime class. Generated parsers inherit from this
// or hold it as a member.

class PegCursor {
public:
    PegCursor() = default;

    void init(const char* input, int length) {
        input_ = input;
        pos_ = input;
        end_ = input + length;
        line_ = 1;
        col_ = 1;
    }

    // ── Save / Restore (PEG backtracking) ───────────────────

    Mark save() const {
        return Mark{pos_, line_, col_};
    }

    void restore(Mark m) {
        pos_ = m.pos;
        line_ = m.line;
        col_ = m.col;
    }

    // ── Position info ───────────────────────────────────────

    int line() const { return line_; }
    int col() const { return col_; }
    bool at_end() const { return pos_ >= end_; }
    const char* pos() const { return pos_; }
    int remaining() const { return static_cast<int>(end_ - pos_); }

    // ── Character matching ──────────────────────────────────

    char peek() const {
        return (pos_ < end_) ? *pos_ : '\0';
    }

    // Match a single character. Returns true and advances on match.
    bool match(char c) {
        if (pos_ < end_ && *pos_ == c) {
            advance();
            return true;
        }
        return false;
    }

    // Match a literal string. Returns true and advances on match.
    bool match(const char* str) {
        int len = static_cast<int>(strlen(str));
        if (pos_ + len > end_) return false;
        if (memcmp(pos_, str, len) != 0) return false;
        for (int i = 0; i < len; i++) advance();
        return true;
    }

    // Match a character class predicate.
    bool match_charset(bool(*fn)(char)) {
        if (pos_ < end_ && fn(*pos_)) {
            advance();
            return true;
        }
        return false;
    }

    // ── Lookahead (no consume) ──────────────────────────────

    bool peek_at(const char* str) const {
        int len = static_cast<int>(strlen(str));
        if (pos_ + len > end_) return false;
        return memcmp(pos_, str, len) == 0;
    }

    bool peek_at(char c) const {
        return pos_ < end_ && *pos_ == c;
    }

    bool peek_charset(bool(*fn)(char)) const {
        return pos_ < end_ && fn(*pos_);
    }

    // ── SPAN optimization ───────────────────────────────────
    // Consume characters while charset matches (tight loop).

    void span(bool(*fn)(char)) {
        while (pos_ < end_ && fn(*pos_)) {
            advance();
        }
    }

    // Scan forward to delimiter, capturing raw content. Advances past
    // delimiter. Skips over C-style `//` line comments and `/* */`
    // block comments — a delimiter inside a comment doesn't terminate
    // the scan. This matters for embedded code blocks like `(. ... .)`
    // where C++ source legitimately contains `.)` in `// comment` text.
    bool scan_to(const char* delim, std::string& out) {
        int dlen = static_cast<int>(strlen(delim));
        const char* start = pos_;
        while (pos_ + dlen <= end_) {
            // Skip a C++ line comment if we encounter `//`.
            if (pos_ + 1 < end_ && pos_[0] == '/' && pos_[1] == '/') {
                while (pos_ < end_ && *pos_ != '\n') advance();
                continue;
            }
            // Skip a C block comment if we encounter `/*`.
            if (pos_ + 1 < end_ && pos_[0] == '/' && pos_[1] == '*') {
                advance(); advance();
                while (pos_ + 1 < end_ &&
                       !(pos_[0] == '*' && pos_[1] == '/')) {
                    advance();
                }
                if (pos_ + 1 < end_) { advance(); advance(); }
                continue;
            }
            if (memcmp(pos_, delim, dlen) == 0) {
                out.assign(start, pos_);
                for (int i = 0; i < dlen; i++) advance();
                return true;
            }
            advance();
        }
        return false;
    }

    // ── Whitespace + comment skipping ───────────────────────
    // Configured per-grammar via set_whitespace / set_comments.

    void set_whitespace(bool(*fn)(char)) {
        ws_fn_ = fn;
    }

    void set_comments(std::vector<CommentSpec> specs) {
        comments_ = std::move(specs);
    }

    void skip() {
        for (;;) {
            // Skip whitespace
            bool skipped = false;
            while (pos_ < end_ && ws_fn_ && ws_fn_(*pos_)) {
                advance();
                skipped = true;
            }
            // Try to skip a comment
            bool found_comment = false;
            for (auto& c : comments_) {
                if (skip_comment(c)) {
                    found_comment = true;
                    break;
                }
            }
            if (!found_comment && !skipped) break;
        }
    }

    // ── Error reporting ─────────────────────────────────────

    void expected(const char* what) {
        errors_.push_back(ParseError{line_, col_,
            std::string("expected ") + what});
    }

    void error(const std::string& msg) {
        errors_.push_back(ParseError{line_, col_, msg});
    }

    const std::vector<ParseError>& errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }

    // Furthest position reached (for better error messages)
    Mark furthest() const { return furthest_; }

protected:
    void advance() {
        if (pos_ >= end_) return;
        if (*pos_ == '\n') {
            line_++;
            col_ = 1;
        } else {
            col_++;
        }
        pos_++;
        // Track furthest position for error reporting
        if (pos_ > furthest_.pos) {
            furthest_ = {pos_, line_, col_};
        }
    }

    // A STRUCTURED skip element (e.g. (@id … ) annotations): from `open`, walk to the matching
    // close counting generic ()-nesting, treating "…" string literals (with \-escapes) as opaque
    // so their parens don't perturb the balance. The `close` delimiter is unused (paren depth ends
    // it). Language-specific awareness (inner comments, identifier validation) is a backend override.
    bool skip_structured(const CommentSpec& spec) {
        int open_len = static_cast<int>(strlen(spec.open));
        if (pos_ + open_len > end_) return false;
        if (memcmp(pos_, spec.open, open_len) != 0) return false;
        for (int i = 0; i < open_len; i++) advance();
        int depth = 1;
        while (pos_ < end_ && depth > 0) {
            char c = *pos_;
            if (c == '"') {                              // string literal: skip with \-escapes
                advance();
                while (pos_ < end_ && *pos_ != '"') {
                    if (*pos_ == '\\' && pos_ + 1 < end_) advance();
                    advance();
                }
                if (pos_ < end_) advance();
            } else if (c == '(') { depth++; advance(); }
            else if (c == ')') { depth--; advance(); }
            else advance();
        }
        return true;
    }

    bool skip_comment(const CommentSpec& spec) {
        if (spec.structured) return skip_structured(spec);
        int open_len = static_cast<int>(strlen(spec.open));
        if (pos_ + open_len > end_) return false;
        if (memcmp(pos_, spec.open, open_len) != 0) return false;

        for (int i = 0; i < open_len; i++) advance();

        int close_len = static_cast<int>(strlen(spec.close));
        // A single-LF close is the line-comment idiom; a line ends at LF or CR (or CRLF),
        // so such a comment terminates on a bare CR too — standard line-comment behavior.
        bool line_close = (close_len == 1 && spec.close[0] == '\n');
        int depth = 1;

        while (pos_ < end_ && depth > 0) {
            if (spec.nested && pos_ + open_len <= end_ &&
                memcmp(pos_, spec.open, open_len) == 0) {
                for (int i = 0; i < open_len; i++) advance();
                depth++;
            } else if (line_close && (*pos_ == '\n' || *pos_ == '\r')) {
                advance();
                depth--;
            } else if (pos_ + close_len <= end_ &&
                       memcmp(pos_, spec.close, close_len) == 0) {
                for (int i = 0; i < close_len; i++) advance();
                depth--;
            } else {
                advance();
            }
        }
        return true;
    }

    static bool is_ident_start(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    static bool is_ident_continue(char c) {
        return is_ident_start(c) || (c >= '0' && c <= '9');
    }

    static bool is_digit(char c) {
        return c >= '0' && c <= '9';
    }

    static bool is_hex_digit(char c) {
        return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    const char* input_ = nullptr;
    const char* pos_ = nullptr;
    const char* end_ = nullptr;
    int line_ = 1;
    int col_ = 1;

    bool(*ws_fn_)(char) = nullptr;
    std::vector<CommentSpec> comments_;
    std::vector<ParseError> errors_;
    Mark furthest_ = {nullptr, 0, 0};
};

} // namespace peg
