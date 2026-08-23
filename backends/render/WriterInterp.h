#pragma once
//
// WriterInterp — the DYNAMIC writer profile: the writer op-list, executed instead of rendered.
//
// `writer_view_cpp.inja` turns the op-list from `lower_writer_ops` into one C++ stencil per
// op; this walks the same list at runtime and makes the same `bbq::writer` calls. Two
// consumers, ONE producer — a second lowering would be a second grammar, which is exactly
// what Nail's "the generator and the parser share one description" rules out.
//
// It exists because the CEK face compiles a grammar at RUNTIME: there is no generated
// `<Rule>_write` to call, so the Python module (and any other dynamic consumer) needs the
// enforcement walk as an interpreter. What it enforces is unchanged — dependent fields
// (array counts, `@rest` window sizes) recomputed into the overlay so `emit()` yields a
// document that re-parses to the edit that was made (PutGet).
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Capture.h"
#include "CaptureCow.h"

namespace bbq::render {

// Enforce the grammar for `rule` over the edited document (`root` indexes `buf`; `zc` is the
// overlay carrying the edits, and receives the recomputed dependent fields), then serialize.
// `functions` is a `lower_writer_ops` op-list. Returns the bytes, or an empty vector if the
// walk failed (an unknown rule, a variant with no matching arm, a malformed op-list); when
// `error` is non-null it receives a reason on failure.
std::vector<uint8_t> run_writer(const nlohmann::json& functions, const std::string& rule,
                                const bbq::FieldCapture* root, const uint8_t* buf, size_t len,
                                bbq::zcow* zc, std::string* error = nullptr);

}  // namespace bbq::render
