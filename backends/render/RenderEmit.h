#pragma once
//
// RenderEmit — the destination-driven emitter for every backend.
//
// Lowers a compiled BBQ kont graph to a flat, backend-neutral op-list (the
// cg_jump linearization) via burg and renders it through an inja template.
// The one codegen path: kont graph -> JSON op-list -> inja. A backend is an
// `Emitter` subclass (name mapper + template + per-backend spelling); the
// concrete subclasses (owning-C reader/writer, C++ ZCow reader) ride one
// render loop. The free functions below are the public facade over them.
//
// The lowering walks each rule's kont graph via the generated
// KontKind / child() / succ() accessors (BURG_NODE_OP / CHILD / SUCC);
// expression sub-trees render to C-expression strings; the spine emits
// op-records that the template turns into idiomatic C / C++.
//
#include <string>

#include <nlohmann/json.hpp>

namespace bbq::render {

struct CompilerCtx;

// Render the C readers for every rule using the inja template at `template_path`.
// Pulls the compiled IR, the AST (for @source blocks), and the name prefix from
// `ctx`. Returns the reader function bodies followed by the spec's @source
// user-code blocks (verbatim, so readers can call them).
std::string render_reader_c(const CompilerCtx& ctx, const std::string& template_path);

// Forward declarations for every rule's reader — `bool x_read(bbq_ctx_t*, x_t*);`.
std::string render_reader_decls(const CompilerCtx& ctx);

// Render the C owning WRITERS for every rule using the inja template at `template_path`
// — the dual of render_reader_c over the SAME burg lowering (OwningCWriter Emitter,
// loads `in->` fields → emits bytes via bbq_write_ctx_t).
std::string render_writer_c(const CompilerCtx& ctx, const std::string& template_path);

// Forward declarations for every rule's writer — per-shape signature
// (`bool x_write(bbq_write_ctx_t*, const x_t*);`, by value for a bare prim rule).
std::string render_writer_decls(const CompilerCtx& ctx);

// Render the cpp-zcow (view) readers: the SAME burg lowering driven through the
// view profile + C++ view template, producing `bbq::CaptureMetadata X_read(...)`
// functions that build the FieldCapture index. The second profile, not a new
// generator. `ns` is the C++ namespace (matches the types header).
std::string render_reader_view(const CompilerCtx& ctx, const std::string& template_path,
                               const std::string& ns);

// Render the c-lite (C view) readers: the SAME burg lowering driven through the C view
// emitter + `reader_view_c.inja`, producing `bbq_capture_metadata <pfx>_<rule>_read(...)`
// functions that build a span index over the input (decode-on-access, no backing store).
// Reader only — no writer, no types/handle header. C uses the name prefix, so no `ns`.
std::string render_reader_view_c(const CompilerCtx& ctx, const std::string& template_path);

// Forward declarations for every rule's c-lite reader — `bbq_capture_metadata
// <pfx>_<rule>_read(const uint8_t*, size_t, bbq_arena*);` (for the generated header).
std::string render_reader_view_c_decls(const CompilerCtx& ctx);

}  // namespace bbq::render
