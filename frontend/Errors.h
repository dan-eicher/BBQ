#pragma once

#include <string>
#include <vector>
#include <cstdarg>
#include <iostream>
#include "BBQ_AST.h"

namespace bbqgen {

struct Error {
    BBQ::SourceLoc loc;
    std::string message;
    bool is_warning = false;
};

class ErrorReporter {
public:
    void error(BBQ::SourceLoc loc, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        add(loc, fmt, args, false);
        va_end(args);
    }

    void warning(BBQ::SourceLoc loc, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        add(loc, fmt, args, true);
        va_end(args);
    }

    bool has_errors() const { return error_count_ > 0; }
    int error_count() const { return error_count_; }

    const std::vector<Error>& errors() const { return errors_; }

    void print_all(std::ostream& os) const {
        for (auto& e : errors_) {
            if (e.is_warning)
                os << "warning";
            else
                os << "error";
            if (e.loc.line > 0)
                os << " (line " << e.loc.line << ", col " << e.loc.column << ")";
            os << ": " << e.message << "\n";
        }
    }

private:
    void add(BBQ::SourceLoc loc, const char* fmt, va_list args, bool warn) {
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        errors_.push_back({loc, std::string(buf), warn});
        if (!warn) ++error_count_;
    }

    std::vector<Error> errors_;
    int error_count_ = 0;
};

} // namespace bbqgen
