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
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Capture.h"
#include "CaptureCow.h"

namespace bbq::render {

// The membership test for C — "does this byte string parse as this rule, all of it?".
//
// Foster et al. Def 3.2 types a well-behaved lens with `put(A × C) ⊆ C` as well as the
// GetPut/PutGet laws, and that typing is the condition the walk cannot establish on its
// own: the walk replays the shape the PARSE recorded, so an edit that changes which shape
// the grammar selects (a switch discriminant, a value a `where` now rejects) leaves it
// emitting bytes that are not in C. PutGet does not catch this — `⊑` holds vacuously when
// its left side is undefined, so a document that fails to parse satisfies PutGet trivially.
// The definitive test is `get` itself, which is why this is a callback: the parser differs
// per face (the CEK machine for a runtime grammar, the generated reader for compiled ones).
// `why` receives the parser's own diagnostic.
using VerifyFn = std::function<bool(const uint8_t* data, size_t len, std::string* why)>;

// Enforce the grammar for `rule` over the edited document (`root` indexes `buf`; `zc` is the
// overlay carrying the edits, and receives the recomputed dependent fields), then serialize.
// `functions` is a `lower_writer_ops` op-list. Returns the bytes, or an empty vector if the
// walk failed (an unknown rule, a variant with no matching arm, a malformed op-list) or if
// `verify` rejected the result; when `error` is non-null it receives a reason on failure.
// Passing no `verify` skips the (PUT) check and only guarantees the derived fields — the
// caller is then claiming membership some other way.
std::vector<uint8_t> run_writer(const nlohmann::json& functions, const std::string& rule,
                                const bbq::FieldCapture* root, const uint8_t* buf, size_t len,
                                bbq::zcow* zc, std::string* error = nullptr,
                                const VerifyFn& verify = {});

// The nodes this grammar DERIVES for `root` — array counts (including path and per-element
// counts) and @rest sizes. Nail: a dependent field is "not exposed in the data model, but
// instead transparently computed", so a face that does expose them can refuse an assignment
// rather than accept it and then overwrite it (a non-vacuous PutGet violation). Runs the
// same walk against a scratch overlay; the document is not touched.
std::vector<const bbq::FieldCapture*> derived_fields(
    const nlohmann::json& functions, const std::string& rule,
    const bbq::FieldCapture* root, const uint8_t* buf, size_t len);

}  // namespace bbq::render
