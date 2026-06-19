#pragma once
//
// RenderTypes — the one type-generator backend (C + C++ over one lowering).
//
// `lower_type_model` lowers the AST (+ Sema) to a single language-NEUTRAL type
// model: type uses are neutral `type_node`s, names are canonical CamelCase. Each
// backend is just a template + name/type mapper callbacks over that one model —
// C spells via CTypeMapper/to_snake_case, C++ uses the CamelCase name as-is. No
// per-language lowering exists; there is exactly one RenderTypes.
//
#include <string>
#include <nlohmann/json.hpp>

namespace BBQ { struct Grammar; }
namespace bbqgen { class Sema; }

namespace bbq::render {

struct CompilerCtx;

// The one shared type-model lowering: AST + Sema → the neutral type model that
// both render_types_c and render_types_cpp render.
nlohmann::json lower_type_model(const CompilerCtx& ctx);

// Render the C owning-types header (inja template at `template_path`): include
// guard, forward-decls, struct/tagged/array/optional/typedef/bitfield defs,
// `_free` functions, @header blocks.
std::string render_types_c(const CompilerCtx& ctx, const std::string& template_path);

// Render the C++ typed-view (ZCow handle class) header, optionally in `ns`.
// Throws std::runtime_error naming the first construct C++ doesn't yet support.
std::string render_types_cpp(const CompilerCtx& ctx,
                             const std::string& template_path,
                             const std::string& ns = "");

}  // namespace bbq::render
