// ddcgc — code-emit backend interface.
//
// One concrete subclass per output language. Each takes a parsed
// + type-checked .ddcg AST plus the loaded schemas, and writes a
// compilable source file that implements the dispatch functions.
//
// Modeled on burgc's pluggable backend pattern. Factory functions
// hide the concrete classes; callers depend only on `Backend`.

#pragma once

#include "ddcg_ast.h"
#include "schema.h"
#include "typecheck.h"

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace ddcgc {

class Backend {
public:
    virtual ~Backend() = default;

    // Emit source for `file` to `out`. Returns false on emission
    // error; appends human-readable messages to `errors`.
    virtual bool emit(const DdcgAst::File* file,
                      const std::map<std::string, Schema>& schemas,
                      const CheckResult& check,
                      std::ostream& out,
                      std::vector<std::string>& errors) = 0;
};

std::unique_ptr<Backend> create_cpp_backend();

} // namespace ddcgc
