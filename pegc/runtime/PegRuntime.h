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
};

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

    // ── Keyword matching ────────────────────────────────────
    // Match literal + verify next char is NOT ident-continue.
    // This is the key context-sensitive keyword mechanism.

    bool keyword(const char* kw) {
        int len = static_cast<int>(strlen(kw));
        if (pos_ + len > end_) return false;
        if (memcmp(pos_, kw, len) != 0) return false;
        // Check that keyword is not followed by ident char
        if (pos_ + len < end_ && is_ident_continue(pos_[len])) return false;
        for (int i = 0; i < len; i++) advance();
        return true;
    }

    // Peek at keyword (no consume).
    bool peek_keyword(const char* kw) const {
        int len = static_cast<int>(strlen(kw));
        if (pos_ + len > end_) return false;
        if (memcmp(pos_, kw, len) != 0) return false;
        if (pos_ + len < end_ && is_ident_continue(pos_[len])) return false;
        return true;
    }

    // ── Built-in token patterns ─────────────────────────────

    // Match identifier: [a-zA-Z_][a-zA-Z0-9_]*
    bool ident(std::string& out) {
        if (pos_ >= end_ || !is_ident_start(*pos_)) return false;
        const char* start = pos_;
        advance();
        while (pos_ < end_ && is_ident_continue(*pos_)) advance();
        out.assign(start, pos_);
        return true;
    }

    // Match C++-style qualified identifier: ident("::" ident)*.  Used by
    // grammars whose symbols can refer to enum values / namespaced
    // constants (e.g. burgc's `TERM Foo = ns::KOP_FOO`).
    bool qualified_ident(std::string& out) {
        if (pos_ >= end_ || !is_ident_start(*pos_)) return false;
        const char* start = pos_;
        advance();
        while (pos_ < end_ && is_ident_continue(*pos_)) advance();
        for (;;) {
            if (pos_ + 1 >= end_ || pos_[0] != ':' || pos_[1] != ':') break;
            const char* save = pos_;
            advance(); advance();  // consume "::"
            if (pos_ >= end_ || !is_ident_start(*pos_)) { pos_ = save; break; }
            advance();
            while (pos_ < end_ && is_ident_continue(*pos_)) advance();
        }
        out.assign(start, pos_);
        return true;
    }

    // Match integer literal: decimal, hex (0x), binary (0b)
    bool integer(int64_t& out) {
        if (pos_ >= end_ || !is_digit(*pos_)) return false;
        const char* start = pos_;
        if (*pos_ == '0' && pos_ + 1 < end_) {
            char next = pos_[1];
            if (next == 'x' || next == 'X') {
                advance(); advance(); // skip 0x
                if (pos_ >= end_ || !is_hex_digit(*pos_)) return false;
                while (pos_ < end_ && is_hex_digit(*pos_)) advance();
                out = static_cast<int64_t>(strtoll(start, nullptr, 16));
                return true;
            }
            if (next == 'b' || next == 'B') {
                advance(); advance(); // skip 0b
                if (pos_ >= end_ || (*pos_ != '0' && *pos_ != '1')) return false;
                while (pos_ < end_ && (*pos_ == '0' || *pos_ == '1')) advance();
                out = static_cast<int64_t>(strtoll(start + 2, nullptr, 2));
                return true;
            }
        }
        while (pos_ < end_ && is_digit(*pos_)) advance();
        out = static_cast<int64_t>(strtoll(start, nullptr, 10));
        return true;
    }

    // Match floating point: digits '.' digits
    bool floating(double& out) {
        if (pos_ >= end_ || !is_digit(*pos_)) return false;
        const char* start = pos_;
        while (pos_ < end_ && is_digit(*pos_)) advance();
        if (pos_ >= end_ || *pos_ != '.') { pos_ = start; return false; }
        advance(); // '.'
        while (pos_ < end_ && is_digit(*pos_)) advance();
        out = strtod(start, nullptr);
        return true;
    }

    // Match string literal: "..." with basic unquoting
    bool string_lit(std::string& out) {
        if (pos_ >= end_ || *pos_ != '"') return false;
        advance(); // skip opening "
        out.clear();
        while (pos_ < end_ && *pos_ != '"') {
            if (*pos_ == '\\' && pos_ + 1 < end_) {
                advance();
                switch (*pos_) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '0': out += '\0'; break;
                    default: out += *pos_; break;
                }
            } else {
                out += *pos_;
            }
            advance();
        }
        if (pos_ < end_) advance(); // skip closing "
        return true;
    }

    // Scan forward to delimiter, capturing raw content. Advances past delimiter.
    bool scan_to(const char* delim, std::string& out) {
        int dlen = static_cast<int>(strlen(delim));
        const char* start = pos_;
        while (pos_ + dlen <= end_) {
            if (memcmp(pos_, delim, dlen) == 0) {
                out.assign(start, pos_);
                for (int i = 0; i < dlen; i++) advance();
                return true;
            }
            advance();
        }
        return false;
    }

    // Match char literal: '.'
    bool char_lit(char& out) {
        if (pos_ >= end_ || *pos_ != '\'') return false;
        advance(); // skip opening '
        if (pos_ >= end_) return false;
        if (*pos_ == '\\' && pos_ + 1 < end_) {
            advance();
            switch (*pos_) {
                case 'n': out = '\n'; break;
                case 'r': out = '\r'; break;
                case 't': out = '\t'; break;
                case '\\': out = '\\'; break;
                case '\'': out = '\''; break;
                case '0': out = '\0'; break;
                default: out = *pos_; break;
            }
        } else {
            out = *pos_;
        }
        advance();
        if (pos_ < end_ && *pos_ == '\'') advance(); // skip closing '
        return true;
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

private:
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

    bool skip_comment(const CommentSpec& spec) {
        int open_len = static_cast<int>(strlen(spec.open));
        if (pos_ + open_len > end_) return false;
        if (memcmp(pos_, spec.open, open_len) != 0) return false;

        for (int i = 0; i < open_len; i++) advance();

        int close_len = static_cast<int>(strlen(spec.close));
        int depth = 1;

        while (pos_ < end_ && depth > 0) {
            if (spec.nested && pos_ + open_len <= end_ &&
                memcmp(pos_, spec.open, open_len) == 0) {
                for (int i = 0; i < open_len; i++) advance();
                depth++;
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
