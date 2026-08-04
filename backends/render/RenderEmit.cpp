#include "RenderEmit.h"

#include "CompilerCtx.h"
#include "CompiledGrammar.h"
#include "BBQ_AST.h"   // BBQ::Grammar / CodeSection (user-code blocks)
#include "Names.h"     // bbqgen::to_snake_case — the canonical name rule (frontend)
#include "bbq_codegen.h"
#include "BbqMatcher.h" // burgc-generated continuation-stencil lowering (bbq_render::BurgMatcher)

#include <inja.hpp>

#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <set>

namespace bbq::render {

using namespace bbq::cek;
using json = nlohmann::json;

namespace {

// Rule name (PascalCase) → C identifier base. Uses the canonical
// bbqgen::to_snake_case (frontend) so reader names match the generated type
// names by construction.
std::string snake(const std::string& s) { return bbqgen::to_snake_case(s); }

// Rule name → C type/function base with the optional name prefix applied.
// Mirrors CTypeMapper::prefixed_name so reader function/type names line up with
// the generated types (`<prefix>_<rule>_t` / `<prefix>_<rule>_read`).
std::string cname(const std::string& rule, const std::string& tp) {
    return tp.empty() ? snake(rule) : tp + "_" + snake(rule);
}

// The C name mapper: the neutral prim descriptor (from bbq_codegen's prim_json)
// → the C runtime reader / scalar type. Registered as inja callbacks so the
// template names things without the lowering baking C-specific strings in.
std::string read_fn_json(const json& p) {
    std::string kind = p["kind"]; int w = p["w"]; bool be = p["be"];
    bool sgn = p["signed"]; std::string enc = p["enc"];
    // Unsuffixed types follow the live endian register at read time (the CEK's
    // PrimEndian::Native) → the suffix-less runtime reader; suffixed types bake be/le.
    bool native = p.value("native", false);
    if (kind == "bool") return "bbq_read_bool";
    if (kind == "float") return std::string("bbq_read_f") + (w == 64 ? "64" : "32") +
                                 (native ? "" : be ? "be" : "le");
    if (enc == "uleb") return w == 64 ? "bbq_read_uleb128_u64" : "bbq_read_uleb128_u32";
    if (enc == "sleb") return w == 64 ? "bbq_read_sleb128_i64" : "bbq_read_sleb128_i32";
    std::string base = sgn ? "bbq_read_i" : "bbq_read_u";
    std::string suf = (w == 8) ? "" : native ? "" : (be ? "be" : "le");
    return base + std::to_string(w) + suf;
}
std::string ctype_json(const json& p) {
    std::string kind = p["kind"]; int w = p["w"]; bool sgn = p["signed"];
    if (kind == "bool") return "bool";
    if (kind == "float") return w == 64 ? "double" : "float";
    return (sgn ? "int" : "uint") + std::to_string(w) + "_t";
}
// The write dual of read_fn_json: the neutral prim descriptor → the C runtime
// writer. Suffix-less (native) prims follow the live endian register at write time
// (the `@endian:` switch), exactly as the suffix-less readers follow it on read.
std::string write_fn_json(const json& p) {
    std::string kind = p["kind"]; int w = p["w"]; bool be = p["be"];
    bool sgn = p["signed"]; std::string enc = p["enc"];
    bool native = p.value("native", false);
    if (kind == "bool") return "bbq_write_bool";
    if (kind == "float") return std::string("bbq_write_f") + (w == 64 ? "64" : "32") +
                                 (native ? "" : be ? "be" : "le");
    if (enc == "uleb") return w == 64 ? "bbq_write_uleb128_u64" : "bbq_write_uleb128_u32";
    if (enc == "sleb") return w == 64 ? "bbq_write_sleb128_i64" : "bbq_write_sleb128_i32";
    std::string base = sgn ? "bbq_write_i" : "bbq_write_u";
    std::string suf = (w == 8) ? "" : native ? "" : (be ? "be" : "le");
    return base + std::to_string(w) + suf;
}

// BBQ_MUSTTAIL: a guaranteed tail call where the compiler supports it (clang,
// gcc 15+), a plain return otherwise. The reader is the compiled CEK — each kont
// is a stencil that tail-calls its successor — so this keeps the chain O(1) stack
// (loops use real C `for`, so they never rely on it).
const char* kMustTail =
    "#if defined(__clang__)\n#  define BBQ_MUSTTAIL [[clang::musttail]]\n"
    "#elif defined(__GNUC__) && (__GNUC__ >= 15)\n#  define BBQ_MUSTTAIL [[gnu::musttail]]\n"
    "#else\n#  define BBQ_MUSTTAIL\n#endif\n";

// ── The codegen Emitter — the one interface every backend rides ─────────────
// ONE render loop (render_emit) lowers each rule's kont graph via burg and renders a
// template; a backend is a SUBCLASS that owns ALL of its own spelling — naming, template,
// name-mappers, pro/epilogue, AND the per-backend output spelling (field refs, builtins,
// path-nav, scope nesting) — as method overrides. There is no separate profile object:
// the Emitter IS the speller (so it can't drift back into a swappable profile). This
// subsumes the old duplicated render_reader_* functions; the writers ride it too.
//
// The spelling defaults below are the owning-C READER (a field is an `out->` lvalue,
// builtins run against the read ctx, scope boundaries are compile-time prefix markers
// the fold resolves). `ZCowReader` overrides what the view spells differently; the
// writers override `out->`→`in->` and the read ctx → the write ctx.
struct Emitter {
    virtual ~Emitter() = default;
    virtual std::string type_prefix() const { return ""; }     // C name prefix (CTypeMapper)
    virtual std::string target_root() const { return ""; }     // root prepended to targets (out->/in->)
    virtual json fn_header(const std::string& rule) const = 0; // {name,[type],kpfx,...}
    virtual std::string template_path() const = 0;
    virtual void register_mappers(inja::Environment&) const = 0;
    // Fold-pass over the lowered op-list before the target-root prepend:
    // the owning reader folds begin_struct/end_struct into the compile-time path and
    // drops them; the view keeps them as runtime stencils (default: no-op).
    virtual void post_lower(json& /*functions*/) const {}
    virtual void extra_data(json&, const CompilerCtx&) const {}
    virtual std::string prologue() const { return ""; }
    virtual std::string epilogue(const CompilerCtx&) const { return ""; }

    // ── output spelling (render_expr dispatches the expr-tree leaves here) ──
    virtual std::string field_ref(const std::string& name) const { return "out->" + name; }
    // A field_ref to a NON-SCALAR (Bytes/String) capture: the owning backend's
    // out->name is already the right type; only the views need a span recovery.
    virtual std::string field_ref_bytes(const std::string& name) const { return field_ref(name); }
    virtual std::string path_start(const std::string& name) const { return "out->" + name; }
    virtual std::string path_field(const std::string& base, const std::string& field) const {
        return base + "." + field;
    }
    virtual std::string path_index(const std::string& base, const std::string& idx) const {
        std::string b = base;
        if (!b.empty() && b.back() == '.') b.pop_back();
        return b + ".items[" + idx + "]";
    }
    virtual std::string path_value(const std::string& expr) const { return expr; }
    virtual std::string cross_ref(const std::string& parent, int depth,
                                  const std::string& path) const {
        return "((" + cname(parent, type_prefix()) + "_t*)bbq_scope_ptr(ctx, " +
               std::to_string(depth) + "))->" + path;
    }
    virtual std::string builtin(const std::string& which) const {
        if (which == "pos")        return "bbq_pos(ctx)";
        if (which == "remaining")  return "bbq_remaining(ctx)";
        if (which == "at_end")     return "bbq_at_end(ctx)";
        if (which == "loop_index") return "bbq_loop_index(ctx)";
        if (which == "end")        return "bbq_effective_end(ctx)";
        if (which == "start")      return "_struct_start";
        if (which == "peek")       return "(bbq_pos(ctx) < bbq_effective_end(ctx) ? (int64_t)ctx->data[bbq_pos(ctx)] : (int64_t)0)";
        if (which == "buffer")     return "((bbq_bytes_t){ctx->data + _struct_start, bbq_pos(ctx) - _struct_start})";
        return "/*?builtin*/";
    }
    // Binary / unary operator spelling. Default = the host operator verbatim. The C view
    // overrides to keep boolean-valued operators bool-TYPED (C yields int for them, the
    // BBQ model — and C++ — says bool), so a `compute`'s kind deduction stays faithful.
    virtual std::string bin_expr(const std::string& op, const std::string& l,
                                 const std::string& r) const {
        return "(" + l + " " + op + " " + r + ")";
    }
    virtual std::string un_expr(const std::string& op, const std::string& x) const {
        return "(" + op + x + ")";
    }
    // The capture index for an array element at nesting `level` — the owning fold spells
    // `…items[<array_index(level)>]`. Owning-reader: the read-ctx loop frame.
    virtual std::string array_index(int level) const {
        return "bbq_loop_index_at(ctx, bbq_cur_loop_base(ctx) + " + std::to_string(level) + ")";
    }
    // Prepend the target root to a folded leaf path. The view's root is empty (identity).
    // For the owning reader/writer an EMPTY leaf is a top-level non-struct rule whose
    // whole value IS the target — `(*out)` / `(*in)`, not the broken `out->`/`in->` — so
    // a bare-prim/optional/array/switch rule roots correctly (greens the top-level reds).
    virtual std::string rooted_target(const std::string& leaf) const {
        std::string r = target_root();                       // "out->" / "in->" / ""
        if (r.empty()) return leaf;
        if (leaf.empty()) return "(*" + r.substr(0, r.size() - 2) + ")";
        return r + leaf;
    }
    // The forward declaration for one rule's generated function — the per-backend
    // spelling (reader `bbq_ctx_t*`/`out` vs writer `bbq_write_ctx_t*`/`const…in`),
    // beside fn_header so the signature can't drift from the definition. The view
    // reader emits no separate decls (its header is self-contained), so default none.
    virtual std::string decl(const std::string& /*rule*/) const { return ""; }

  protected:
    // ── Shared post_lower transforms (defined just below), composed per concrete impl.
    // Base methods — the Emitter IS the speller, not loose free functions — and their
    // reuse cross-cuts (owning-fold spans reader+writer; writer-derive spans C+C++), so
    // they live on the base, not a Reader/Writer tier. `fold_owning_paths` uses the
    // virtual `array_index()` directly.
    void fold_owning_paths(json& functions) const;       // owning reader + owning writer
    void derive_write_prefixes(json& functions) const;   // C writer + C++ writer
};

// A view resolves a cross-rule reference the way it resolves any other name. Its
// capture index IS the scope chain — closing a scope packs that scope's fields out
// of the open list — so a lookup walks innermost-to-outermost and the path selects
// the binding, exactly like a lexical lookup. `parent` and `depth` are the owning
// backends' scope-pointer machinery; a scope chain needs neither. A sibling's field
// is not in the chain and has to be spelled as a path, which is what the resolved
// path already is.
static std::string cross_ref_via_path(const Emitter& em, const std::string& path) {
    size_t dot = path.find('.');
    std::string e = em.path_start(path.substr(0, dot));
    while (dot != std::string::npos) {
        size_t next = path.find('.', dot + 1);
        e = em.path_field(e, path.substr(dot + 1, next == std::string::npos
                                                  ? std::string::npos : next - dot - 1));
        dot = next;
    }
    return em.path_value(e);
}

// The owning-C path machine. burg emits bare leaf targets plus neutral
// begin_struct/end_struct/array boundary ops; this fold walks each function's ops in
// program order, reconstructing the compile-time path prefix (struct fields `f.` /
// switch-case unions `f.u.case_N.` / optional `.value.` / array element slots
// `.items[i]`), writing it into every target, then drops the boundary ops. The owning
// reader and the owning writer share it; only the target root (`out->` vs `in->`) and the
// per-element loop index (`array_index`, the read-ctx loop frame vs the write-ctx one)
// differ, so those are supplied by the Emitter — the rest of the path-building is shared.
void Emitter::fold_owning_paths(json& functions) const {
    auto container = [](const std::string& p) {  // nested-rule invoke slot: strip trailing '.'
        return (!p.empty() && p.back() == '.') ? p.substr(0, p.size() - 1) : p;
    };
    auto prepend = [](json& op, const char* key, const std::string& pfx) {
        if (op.contains(key)) op[key] = pfx + op[key].get<std::string>();
    };
    // A same-scope reference inside an EXPRESSION names its field the way the grammar
    // wrote it, so it needs the same compile-time path a target gets: a `where` on a
    // bitfield entry says `acc_extended`, but the field lives at `flags.acc_extended`.
    // cross_ref carries its own path off the scope pointer and is left alone.
    std::function<void(json&, const std::string&)> qualify_refs =
        [&](json& n, const std::string& pfx) -> void {
        if (n.is_array()) { for (auto& c : n) qualify_refs(c, pfx); return; }
        if (!n.is_object()) return;
        if (n.value("e", std::string()) == "field_ref") {
            n["name"] = pfx + n["name"].get<std::string>();
            return;
        }
        for (auto& c : n) qualify_refs(c, pfx);
    };
    auto is_boundary = [](const std::string& k) {
        return k == "begin_struct" || k == "end_struct" || k == "end_array";
    };
    for (auto& fn : functions) {
        // ── Pass 1: index the ops; build the boundary-successor resolver, the scalar
        // switch-arm slot map, and the optional present-element set — all from the neutral
        // structure burg emits (no backend tags), before any boundary is dropped.
        std::unordered_map<int, const json*> by_id;
        for (auto& op : fn["ops"]) by_id[op["id"].get<int>()] = &op;
        // resolve(id): a successor that lands on a (soon-dropped) boundary forwards along
        // `next` to the next real op — transitively, since boundaries chain at scope close
        // (end_array→end_struct→…). -1 (no successor) is identity, as is any non-boundary.
        std::function<int(int)> resolve = [&](int id) -> int {
            std::unordered_set<int> seen;
            while (id >= 0) {
                auto it = by_id.find(id);
                if (it == by_id.end() || !is_boundary(it->second->value("op", std::string()))) break;
                if (!seen.insert(id).second) break;                 // cycle guard (balanced scopes)
                id = it->second->value("next", -1);
            }
            return id;
        };
        // A SCALAR switch arm's case target is a leaf read → its owning slot is
        // `<field>.u.<member>` (struct/ruleref arms target a begin_struct, handled by its
        // own case_member, so the leaf bake below skips begin_struct).
        std::unordered_map<int, std::pair<std::string, std::string>> sw_arm;
        // Optional present-element reads that store into `<field>.value`. The
        // optional/optional_prim/begin_optional ops carry the BARE field (the template
        // spells .has_value/.value); it is their present/inner read that needs `.value`.
        std::unordered_set<int> opt_value_ids;
        for (auto& op : fn["ops"]) {
            std::string k = op.value("op", std::string());
            if (k == "switch") {
                std::string field = op.value("tag_target", std::string());
                for (auto& c : op["cases"])
                    sw_arm[c["target"].get<int>()] = { field, "case_" + std::to_string(c["ordinal"].get<int>()) };
                if (op.value("has_default", false) && !op.value("default_reject", false))
                    sw_arm[op["default_target"].get<int>()] = { field, "default_val" };
            } else if (k == "optional") {
                opt_value_ids.insert(op["present"].get<int>());
            } else if (k == "begin_optional") {
                opt_value_ids.insert(op["inner"].get<int>());
            }
        }
        // ── Pass 2: program-order walk — reconstruct the compile-time path prefix (struct
        // fields `f.` / switch-union `f.u.case_N.` via begin_struct's case_member / optional
        // `.value.` / array slots `.items[i]`), bake the scalar-arm + optional-value leaf,
        // and drop the boundary ops.
        std::string prefix;
        // A reference inside an EXPRESSION resolves in the STRUCT nesting it was written
        // in, which is not the target prefix: an array's element slot (`f.items[i].`)
        // extends the target path but not the scope a `where`/interval expression sees,
        // so `@[base + @index]` on an element still means the enclosing struct's `base`.
        std::string struct_prefix;
        std::vector<std::string> scope, sscope, array_stack;  // pushed/popped together
        int loop_level = 0;
        json kept = json::array();
        for (auto& op : fn["ops"]) {
            std::string k = op.value("op", std::string());
            if (k == "begin_struct") {
                scope.push_back(prefix);
                sscope.push_back(struct_prefix);
                if (op.contains("case_member")) {
                    std::string cf = op.value("case_field", std::string());
                    prefix += (cf.empty() ? "" : cf + ".") + "u." +
                              op["case_member"].get<std::string>() + ".";
                }
                else if (op.value("opt_value", false)) {
                    std::string nm = op.value("name", std::string());
                    prefix += (nm.empty() ? "value." : nm + ".value.");
                }
                else { std::string nm = op.value("name", std::string());
                       if (!nm.empty()) prefix += nm + "."; }
                struct_prefix = prefix;
                continue;  // boundary: drop
            }
            if (k == "end_struct") {
                prefix = scope.back(); scope.pop_back();
                struct_prefix = sscope.back(); sscope.pop_back();
                continue;
            }
            if (k == "end_array") {
                prefix = scope.back(); scope.pop_back();
                struct_prefix = sscope.back(); sscope.pop_back();
                loop_level--; array_stack.pop_back();
                continue;
            }
            if (k == "array_begin" || k == "array_begin_grow" || k == "array_begin_resync") {
                // An array that IS an optional's present element lives in `<field>.value`,
                // the same retarget the leaf bake below applies to scalar elements.
                std::string leaf = op["field"].get<std::string>();
                if (opt_value_ids.count(op["id"].get<int>()))
                    leaf = leaf.empty() ? "value" : leaf + ".value";
                std::string full = prefix + leaf;
                op["field"] = full;
                scope.push_back(prefix);
                sscope.push_back(struct_prefix);
                array_stack.push_back(full);
                prefix = full + ".items[" + array_index(loop_level) + "]";
                loop_level++;
                kept.push_back(op);
                continue;
            }
            if (k == "array_next_grow") { op["field"] = array_stack.back(); kept.push_back(op); continue; }
            if (k == "invoke") {
                op["target"] = container(prefix);
                op["scope"] = fn.value("scope", true);
                kept.push_back(op); continue;
            }
            // Leaf bake (BEFORE the prefix-prepend): a scalar switch arm becomes its union
            // slot; an optional present-element gains `.value` (empty leaf → bare `value`).
            int id = op["id"].get<int>();
            if (op.contains("target")) {
                auto sa = sw_arm.find(id);
                if (sa != sw_arm.end()) {
                    const std::string& cf = sa->second.first;
                    op["target"] = (cf.empty() ? "" : cf + ".") + "u." + sa->second.second;
                }
                if (opt_value_ids.count(id)) {
                    std::string t = op["target"].get<std::string>();
                    op["target"] = t.empty() ? "value" : t + ".value";
                }
            }
            if (!struct_prefix.empty()) qualify_refs(op, struct_prefix);
            prepend(op, "target", prefix);
            prepend(op, "tag_target", prefix);
            prepend(op, "field", prefix);
            if (op.contains("members")) for (auto& m : op["members"]) prepend(m, "target", prefix);
            if (op.contains("arms"))    for (auto& a : op["arms"])    prepend(a, "target", prefix);
            kept.push_back(op);
        }
        // ── Pass 3: remap every successor that pointed at a dropped boundary to its
        // resolved real target (identity on non-boundaries, so applied uniformly).
        for (auto& op : kept) {
            for (const char* key : {"next", "body", "end", "inner", "present", "absent", "default_target"})
                if (op.contains(key) && op[key].is_number_integer())
                    op[key] = resolve(op[key].get<int>());
            if (op.contains("cases"))
                for (auto& c : op["cases"])
                    if (c.contains("target")) c["target"] = resolve(c["target"].get<int>());
        }
        if (fn.contains("entry") && fn["entry"].is_number_integer())
            fn["entry"] = resolve(fn["entry"].get<int>());
        fn["ops"] = std::move(kept);
    }
}

// OwningCWriter-only post_lower: re-derive the owning writer's content-driven size
// prefixes (count placeholders, @rest window size, until-pad) from the NEUTRAL op-list.
// burg emits no writer tags — these are the owning writer's serialization strategy, not a
// neutral structural fact. Runs BEFORE fold_owning_paths, so leaf targets are still bare
// names (matching the count/@rest field refs), exactly the view burg's tiling actions had.
// The reader/view never run this pass, so they never see (and never saw) these tags.
void Emitter::derive_write_prefixes(json& functions) const {
    // A `[n]` count is a field_ref; a `[h.n]` count is a path_value over a path_field.
    auto count_field = [](const json& e) -> std::string {
        if (e.value("e", std::string()) == "field_ref") return e.value("name", std::string());
        if (e.value("e", std::string()) == "path_value") {
            const json& x = e["x"];
            if (x.value("e", std::string()) == "path_field") return x.value("field", std::string());
        }
        return "";
    };
    for (auto& fn : functions) {
        json& ops = fn["ops"];
        std::unordered_map<int, const json*> by_id;
        for (auto& op : ops) by_id[op["id"].get<int>()] = &op;
        // count ids are per-function (and small): the C writer keys a fixed-size count_holes[]
        // array by id, and an array's count is always SET (at array_begin) before the walk
        // descends into its element bodies — so a nested rule reusing the same id can't clobber
        // an outer count that is already resolved.
        int count_seq = 0;
        std::vector<json> rest_stack;  // per open interval: the @rest size prim, or null
        for (size_t i = 0; i < ops.size(); i++) {
            json& op = ops[i];
            std::string k = op.value("op", std::string());
            if (k == "push_interval") {
                // @rest: a relative window sized by the field just read (the immediately
                // preceding op). The writer skips the push (the rest defines its own
                // extent) and reserves a size hole at that read, backpatched at pop.
                json rest_prim;
                const json& e = op["expr"];
                if (op.value("relative", false) && i > 0 &&
                    e.is_object() && e.value("e", std::string()) == "field_ref") {
                    json& prev = ops[i - 1];
                    if (prev.value("op", std::string()) == "read" &&
                        prev.value("target", std::string()) == e.value("name", std::string())) {
                        prev["rest_reserve"] = true;
                        rest_prim = prev["prim"];
                        op["rest"] = true;
                    }
                }
                rest_stack.push_back(rest_prim);
            }
            else if (k == "pop_interval") {
                if (!rest_stack.empty()) {
                    json rp = rest_stack.back(); rest_stack.pop_back();
                    if (!rp.is_null()) { op["rest"] = true; op["prim"] = rp; }
                }
            }
            else if ((k == "array_begin" || k == "array_begin_resync") && op.contains("count")) {
                // A `[n]`/count(n) over a plain field: the writer serializes the array's
                // REAL length (a re-sized vec → a valid binary), so reserve a placeholder
                // at that count field's read and patch it with field.count at the array.
                std::string cf = count_field(op["count"]);
                if (!cf.empty())
                    for (size_t j = i; j-- > 0; ) {
                        json& r = ops[j];
                        if (r.value("op", std::string()) == "read" && r.value("target", std::string()) == cf) {
                            int cid = count_seq++;
                            r["count_reserve"] = true; r["count_id"] = cid;
                            op["count_patch"] = true; op["count_prim"] = r["prim"]; op["count_id"] = cid;
                            break;
                        }
                    }
            }
            else if (k == "array_next_grow" && op.contains("cond")) {
                // until-mode trailing context: a buffer-terminal `@remaining </<= K`
                // terminator reserves K bytes after the array so a stored count round-trips.
                // cond = !(until); recover K from the neutral until predicate.
                const json& c = op["cond"];
                if (c.value("e", std::string()) == "un" && c.value("op", std::string()) == "!" &&
                    c["x"].value("e", std::string()) == "bin") {
                    const json& u = c["x"];
                    std::string bop = u.value("op", std::string());
                    auto is_rem = [](const json& n){ return n.value("e", std::string()) == "builtin" &&
                                                            n.value("name", std::string()) == "remaining"; };
                    auto as_int = [](const json& n, int64_t& v){
                        if (n.value("e", std::string()) == "int") { v = n["v"].get<int64_t>(); return true; } return false; };
                    int reserve = -1; int64_t kv = 0;
                    if ((bop == "<" || bop == "<=") && is_rem(u["l"]) && as_int(u["r"], kv))
                        reserve = (bop == "<") ? (int)(kv - 1) : (int)kv;
                    else if ((bop == ">" || bop == ">=") && is_rem(u["r"]) && as_int(u["l"], kv))
                        reserve = (bop == ">") ? (int)(kv - 1) : (int)kv;
                    if (reserve > 0) {
                        // buffer-terminal iff the loop exit falls (through end_array) to a return.
                        int e = op.value("end", -1);
                        while (e >= 0) {
                            auto it = by_id.find(e);
                            if (it == by_id.end()) break;
                            std::string ek = it->second->value("op", std::string());
                            if (ek == "end_array") { e = it->second->value("next", -1); continue; }
                            if (ek == "return") op["pad_after"] = reserve;
                            break;
                        }
                    }
                }
            }
        }
    }
}


// Spell a neutral expr record (`{"e": kind, ...}`, built backend-blind by burg) into a
// C/C++ expr string. The combinators (bin/un/tern/call/literals) are backend-neutral;
// only the LEAVES (builtins/refs/path-nav) dispatch to the Emitter — so a backend swap
// is a speller swap, the lowering never re-runs.
std::string render_expr(const json& e, const Emitter& em) {
    const std::string k = e.value("e", std::string());
    if (k == "int")        return std::to_string(e["v"].get<int64_t>());
    if (k == "bool")       return e["v"].get<bool>() ? "1" : "0";
    if (k == "float")      return std::to_string(e["v"].get<double>());
    if (k == "str")        return "\"" + e["v"].get<std::string>() + "\"";
    if (k == "builtin")    return em.builtin(e["name"].get<std::string>());
    if (k == "field_ref")  return e.value("bytes", false)
                                  ? em.field_ref_bytes(e["name"].get<std::string>())
                                  : em.field_ref(e["name"].get<std::string>());
    if (k == "cross_ref")  return em.cross_ref(e["parent"].get<std::string>(),
                                               e["depth"].get<int>(), e["path"].get<std::string>());
    if (k == "path_start") return em.path_start(e["name"].get<std::string>());
    if (k == "path_field") return em.path_field(render_expr(e["base"], em), e["field"].get<std::string>());
    if (k == "path_index") return em.path_index(render_expr(e["base"], em), render_expr(e["idx"], em));
    if (k == "path_value") return em.path_value(render_expr(e["x"], em));
    if (k == "bin") return em.bin_expr(e["op"].get<std::string>(),
                                       render_expr(e["l"], em), render_expr(e["r"], em));
    if (k == "un")  return em.un_expr(e["op"].get<std::string>(), render_expr(e["x"], em));
    if (k == "tern") return "(" + render_expr(e["c"], em) + " ? " + render_expr(e["t"], em) +
                            " : " + render_expr(e["e2"], em) + ")";
    if (k == "call") {
        std::string s = e["func"].get<std::string>() + "(";
        const json& args = e["args"];
        for (size_t i = 0; i < args.size(); i++) { if (i) s += ", "; s += render_expr(args[i], em); }
        return s + ")";
    }
    return "/*?expr*/";
}

// Replace every neutral expr node ({"e":...}) anywhere in an op record with its spelled
// string. Walks the op generically (expr-bearing keys vary: expr/cond/disc/len/count).
void render_exprs_in(json& v, const Emitter& em) {
    if (v.is_object()) {
        if (v.contains("e")) { v = render_expr(v, em); return; }
        for (auto& [k, child] : v.items()) render_exprs_in(child, em);
    } else if (v.is_array()) {
        for (auto& child : v) render_exprs_in(child, em);
    }
}

// burg lowers each rule's kont graph to ONE backend-neutral op-list — a pure function
// of the grammar, with NO Emitter input. Every emitter consumes the same list (boundary
// records threaded + id'd, bare leaf names); each specializes it via post_lower +
// spelling. `fn["rule"]` carries the rule name so render_emit can merge the per-emitter
// function header.
json lower_grammar(const CompilerCtx& ctx) {
    CompiledGrammar* g = ctx.ir;
    json functions = json::array();
    for (int i = 0; i < g->rule_count; i++) {
        bbq_render::BurgMatcher m;
        m.emit.default_le = ctx.default_le();
        m.burg_rewrite(g->rules[i].entry);
        json fn = json::object();
        fn["rule"]  = g->rules[i].name;
        fn["entry"] = m.emit.next_id(g->rules[i].entry);
        fn["ops"]   = m.emit.nodes;
        // Only a struct rule is an addressable scope: Sema counts a cross-rule
        // reference's depth over enclosing STRUCT rules alone, so a switch that
        // pushed a frame would shift every reference below it by one.
        fn["scope"] = false;
        for (auto* r : ctx.ast->rules)
            if (r->name == g->rules[i].name)
                fn["scope"] = (dynamic_cast<BBQ::Struct*>(r->body) != nullptr);
        functions.push_back(std::move(fn));
    }
    return functions;
}

// A field_ref to a Bytes/String capture must spell a span recovery, not an int
// decode — but the capture type isn't on the ref node. Collect each rule's
// read_bytes targets and tag matching field_ref nodes `{"bytes":true}`; the
// dispatch in render_expr then routes them to field_ref_bytes (the CEK semantics:
// a ref yields the field's value AT ITS CAPTURE TYPE).
static void tag_bytes_refs(json& node, const std::set<std::string>& bf) {
    if (node.is_object()) {
        if (node.value("e", std::string()) == "field_ref" &&
            bf.count(node.value("name", std::string())))
            node["bytes"] = true;
        for (auto it = node.begin(); it != node.end(); ++it) tag_bytes_refs(it.value(), bf);
    } else if (node.is_array()) {
        for (auto& el : node) tag_bytes_refs(el, bf);
    }
}
// A `where` on a bitfield entry belongs INSIDE the container read, so collapse_bitfield
// keeps the run going across it and tags the member with the constraint op's id. Move
// that op's expression onto the member and drop it. This is a property of the lowering,
// not of one backend's path folding: the continuation extracts are absorbed for every
// emitter, so an emitter that still saw the standalone op would tail-call an id that
// emits nothing.
static void fold_bitfield_constraints(json& functions) {
    for (auto& fn : functions) {
        std::unordered_map<int, const json*> by_id;
        for (auto& op : fn["ops"]) by_id[op["id"].get<int>()] = &op;
        std::unordered_set<int> folded;
        for (auto& op : fn["ops"]) {
            if (op.value("op", std::string()) != "bitfield") continue;
            for (auto& mem : op["members"]) {
                if (!mem.contains("where_id")) continue;
                auto it = by_id.find(mem["where_id"].get<int>());
                if (it != by_id.end()) {
                    mem["where"] = (*it->second)["expr"];
                    mem["where_msg"] = (*it->second)["msg"];
                    folded.insert(mem["where_id"].get<int>());
                }
                mem.erase("where_id");
            }
        }
        if (folded.empty()) continue;
        json kept = json::array();
        for (auto& op : fn["ops"])
            if (!folded.count(op["id"].get<int>())) kept.push_back(op);
        fn["ops"] = std::move(kept);
    }
}

static void annotate_bytes_field_refs(json& functions) {
    for (auto& fn : functions) {
        std::set<std::string> bf;
        for (auto& op : fn["ops"])
            if (op.value("op", std::string()) == "read_bytes" && op.contains("target"))
                bf.insert(op["target"].get<std::string>());
        if (bf.empty()) continue;
        for (auto& op : fn["ops"]) tag_bytes_refs(op, bf);
    }
}

std::string render_emit(const CompilerCtx& ctx, const Emitter& em, json functions) {
    // Merge the per-emitter function header (name/type/kpfx) onto the neutral lowering,
    // then drop the rule-name stash so it doesn't leak into the template data.
    for (auto& fn : functions) {
        json h = em.fn_header(fn["rule"].get<std::string>());
        for (auto it = h.begin(); it != h.end(); ++it) fn[it.key()] = it.value();
        fn["rule_name"] = fn["rule"];   // kept for failure messages
        fn.erase("rule");
    }
    // Fold-pass: the owning reader folds struct boundaries into the path and drops the
    // begin_struct/end_struct ops (remapping successors); the view leaves them as
    // runtime stencils (its post_lower is a no-op).
    fold_bitfield_constraints(functions);
    annotate_bytes_field_refs(functions);
    em.post_lower(functions);
    // A parse failure has to name the field it happened at. burg builds the message
    // from the BARE leaf, which is empty for anything nested, so a reader that
    // rejected a deeply-buried field could only say ": read failed". Now that the
    // fold has resolved the real path, put it back.
    for (auto& fn : functions) {
        std::string rule = fn.value("rule_name", std::string());
        for (auto& op : fn["ops"]) {
            if (!op.contains("msg")) continue;
            std::string m = op["msg"].get<std::string>();
            std::string t = op.value("target", std::string());
            if (t.empty()) t = op.value("field", std::string());
            if (!t.empty() && !m.empty() && m[0] == ':')
                m = t + m;                      // "<path>: read failed"
            else if (!t.empty() && m.rfind(t, 0) != 0)
                m = t + ": " + m;
            op["msg"] = rule.empty() ? m : rule + "." + m;
        }
    }
    // Spell the neutral expr records: burg emitted backend-blind expr trees; the
    // Emitter spells the leaves (builtins/refs/path-nav) here.
    for (auto& fn : functions)
        for (auto& op : fn["ops"])
            render_exprs_in(op, em);
    // The Emitter supplies the target root: burg emits rootless paths; prepend the root
    // (out->/in->) here so the SAME records serve reader and writer. The view's root is
    // empty (rooted_target is identity); the writer derefs the whole-value root.
    for (auto& fn : functions)
        for (auto& op : fn["ops"]) {
            // Field-path keys (NOT switch cases[].target — those are numeric ids).
            if (op.contains("target"))     op["target"]     = em.rooted_target(op["target"].get<std::string>());
            if (op.contains("tag_target")) op["tag_target"] = em.rooted_target(op["tag_target"].get<std::string>());
            if (op.contains("field"))      op["field"]      = em.rooted_target(op["field"].get<std::string>());
            if (op.contains("members"))
                for (auto& mem : op["members"])
                    if (mem.contains("target")) mem["target"] = em.rooted_target(mem["target"].get<std::string>());
            if (op.contains("arms"))
                for (auto& arm : op["arms"])
                    if (arm.contains("target")) arm["target"] = em.rooted_target(arm["target"].get<std::string>());
        }
    json data;
    data["functions"] = functions;
    data["default_le"] = ctx.default_le();
    em.extra_data(data, ctx);

    std::ifstream tf(em.template_path());
    std::stringstream ss; ss << tf.rdbuf();
    inja::Environment env;
    env.set_trim_blocks(true);
    env.set_lstrip_blocks(true);
    em.register_mappers(env);
    return em.prologue() + env.render(ss.str(), data) + em.epilogue(ctx);
}

// Regular-C owning reader: copy-into-struct, `out->` targets, musttail stencils,
// @source user blocks appended so readers can call them.
struct OwningCReader : Emitter {
    std::string tp_, dir_;
    OwningCReader(std::string tp, std::string dir) : tp_(std::move(tp)), dir_(std::move(dir)) {}
    std::string type_prefix() const override { return tp_; }
    std::string target_root() const override { return "out->"; }
    std::string template_path() const override { return dir_ + "/reader_c_stencil.inja"; }
    json fn_header(const std::string& rule) const override {
        std::string base = cname(rule, tp_);
        return {{"name", base + "_read"}, {"type", base + "_t"}, {"kpfx", base}};
    }
    std::string decl(const std::string& rule) const override {
        std::string base = cname(rule, tp_);
        return "bool " + base + "_read(bbq_ctx_t* ctx, " + base + "_t* out);";
    }
    void register_mappers(inja::Environment& env) const override {
        env.add_callback("read_fn", 1, [](inja::Arguments& a) { return read_fn_json(*a[0]); });
        env.add_callback("ctype",   1, [](inja::Arguments& a) { return ctype_json(*a[0]); });
        std::string tp = tp_;
        env.add_callback("kfn", 1, [tp](inja::Arguments& a) { return cname(a[0]->get<std::string>(), tp); });
    }
    void post_lower(json& functions) const override { fold_owning_paths(functions); }
    std::string prologue() const override { return kMustTail; }
    // @source DEFINITIONS go at the TOP of the reader (before the rules call them, via
    // {{ user_source }} in the template), so a file-local `static` helper needs no @header.
    // @header stays a convenience for a single-file grammar's shared structs (types header).
    void extra_data(json& data, const CompilerCtx& ctx) const override {
        std::string src;
        if (ctx.ast)
            for (auto* cb : ctx.ast->codes)
                if (cb->section == BBQ::CodeSection::SourceBlock) src += std::string(cb->code) + "\n";
        data["user_source"] = src;
    }
};

// Regular-C owning WRITER: the dual of OwningCReader over the SAME folded records —
// loads `in->` fields and emits bytes via the write ctx. Same owning fold, same path
// logic; only the algebra (write vs read), the root (`in->`), and the machine target
// (the write ctx) differ. Rides the kont records — NOT an AST walker (no AST access at
// all; the extern's write func rides ExternalCallNode like its read func).
struct OwningCWriter : Emitter {
    std::string tp_, dir_;
    OwningCWriter(std::string tp, std::string dir) : tp_(std::move(tp)), dir_(std::move(dir)) {}
    std::string type_prefix() const override { return tp_; }
    std::string target_root() const override { return "in->"; }
    std::string template_path() const override { return dir_ + "/writer_c.inja"; }
    // Uniform `const T* in` entry — the dual of the reader's uniform `T* out` (a bare-prim
    // rule's whole value is `(*in)`, set by rooted_target's empty-leaf deref).
    json fn_header(const std::string& rule) const override {
        std::string base = cname(rule, tp_);
        return {{"name", base + "_write"}, {"type", base + "_t"}, {"kpfx", base}};
    }
    std::string decl(const std::string& rule) const override {
        std::string base = cname(rule, tp_);
        return "bool " + base + "_write(bbq_write_ctx_t* ctx, const " + base + "_t* in);";
    }
    void register_mappers(inja::Environment& env) const override {
        env.add_callback("write_fn", 1, [](inja::Arguments& a) { return write_fn_json(*a[0]); });
        env.add_callback("ctype",    1, [](inja::Arguments& a) { return ctype_json(*a[0]); });
        std::string tp = tp_;
        env.add_callback("kfn", 1, [tp](inja::Arguments& a) { return cname(a[0]->get<std::string>(), tp); });
    }
    void post_lower(json& functions) const override {
        // Writer-only: derive the content-driven size prefixes (BEFORE the fold, while
        // leaf targets are still the bare names the count/@rest refs match), then fold
        // the owning struct path like the reader.
        derive_write_prefixes(functions);
        fold_owning_paths(functions);
    }
    std::string prologue() const override { return kMustTail; }
    std::string epilogue(const CompilerCtx& ctx) const override {
        std::string out;
        if (ctx.ast)
            for (auto* cb : ctx.ast->codes)
                if (cb->section == BBQ::CodeSection::WriterBlock) out += std::string(cb->code) + "\n";
        return out;
    }

    // ── write-side spelling (load in->, against the write ctx) ──
    std::string field_ref(const std::string& name) const override { return "in->" + name; }
    std::string path_start(const std::string& name) const override { return "in->" + name; }
    std::string cross_ref(const std::string& parent, int depth,
                          const std::string& path) const override {
        return "((const " + cname(parent, type_prefix()) + "_t*)bbq_w_scope_ptr(ctx, " +
               std::to_string(depth) + "))->" + path;
    }
    std::string builtin(const std::string& which) const override {
        if (which == "pos")        return "bbq_write_pos(ctx)";
        if (which == "loop_index") return "bbq_w_loop_index(ctx)";
        // The remaining builtins (@remaining/@end/@start/@peek/buffer/@at_end) appear only
        // in read-side constraints/array-conditions/discriminants, which the structure-
        // driven writer never emits — spell them inertly so a stray reference still builds.
        if (which == "end")        return "bbq_write_pos(ctx)";
        return "0";
    }
    std::string array_index(int level) const override {
        return "bbq_w_loop_index_at(ctx, bbq_w_cur_loop_base(ctx) + " + std::to_string(level) + ")";
    }
};

}  // namespace

// Regular-C owning reader — one Emitter subclass over the shared render_emit loop.
// `templates_dir` is the template directory; the subclass owns its own filename.
std::string render_reader_c(const CompilerCtx& ctx, const std::string& templates_dir) {
    return render_emit(ctx, OwningCReader(ctx.type_prefix(), templates_dir), lower_grammar(ctx));
}

// Regular-C owning WRITER — the writer twin, same render_emit loop, OwningCWriter
// subclass (burg-driven, the dual of render_reader_c).
std::string render_writer_c(const CompilerCtx& ctx, const std::string& templates_dir) {
    return render_emit(ctx, OwningCWriter(ctx.type_prefix(), templates_dir), lower_grammar(ctx));
}

// Forward declarations for every rule's writer — the OwningCWriter's own decl spelling
// (`bool x_write(bbq_write_ctx_t*, const x_t*);`), the uniform-pointer dual of
// render_reader_decls.
std::string render_writer_decls(const CompilerCtx& ctx) {
    OwningCWriter w(ctx.type_prefix(), "");
    CompiledGrammar* g = ctx.ir;
    std::string out;
    for (int i = 0; i < g->rule_count; i++) out += w.decl(g->rules[i].name) + "\n";
    return out;
}

namespace {
// View-profile (cpp-zcow) atom mappers: the neutral prim descriptor → the C++
// span-record spelling. prim_bytes is the fixed-width advance; capture_expr is the
// runtime CaptureType (native prims resolve against the live endian register).
std::string prim_bytes_json(const json& p) {
    // Fixed-width advance. LEB (variable-length) prims don't use this — the view
    // template routes enc==uleb/sleb to r.uleb_capture/sleb_capture instead.
    return std::to_string(p["w"].get<int>() / 8);
}
std::string capture_expr_json(const json& p) {
    std::string kind = p["kind"]; int w = p["w"];
    bool sgn = p["signed"]; bool be = p["be"]; bool native = p.value("native", false);
    std::string bs = be ? "true" : "false", ns = native ? "true" : "false";
    if (kind == "bool")  return "bbq::CaptureType::Bool";
    if (kind == "float") return "r.float_capture(" + std::to_string(w) + ", " + bs + ", " + ns + ")";
    return "r.int_capture(" + std::to_string(w) + ", " + (sgn ? "true" : "false") +
           ", " + bs + ", " + ns + ")";
}

// The c-lite (C view) twin of capture_expr_json: the neutral prim descriptor → a C
// runtime capture-type expression (bbq_view_int_capture/float_capture over the view
// ctx; native resolves against the live endian register). prim_bytes is shared (w/8).
std::string capture_expr_c_json(const json& p) {
    std::string kind = p["kind"]; int w = p["w"];
    bool sgn = p["signed"]; bool be = p["be"]; bool native = p.value("native", false);
    std::string bs = be ? "true" : "false", ns = native ? "true" : "false";
    if (kind == "bool")  return "BBQ_CT_Bool";
    if (kind == "float") return "bbq_view_float_capture(ctx, " + std::to_string(w) + ", " + bs + ", " + ns + ")";
    return "bbq_view_int_capture(ctx, " + std::to_string(w) + ", " + (sgn ? "true" : "false") +
           ", " + bs + ", " + ns + ")";
}

// C++ cpp-zcow (view) reader: records spans into the FieldCapture index. Same Emitter
// base/loop as OwningCReader, overriding the spelling for decode-on-access + the view
// template/mappers, plus the extern forward-decl list (the view ABI) as backend data.
struct ZCowReader : Emitter {
    std::string ns_, dir_;
    ZCowReader(std::string ns, std::string dir) : ns_(std::move(ns)), dir_(std::move(dir)) {}
    std::string template_path() const override { return dir_ + "/reader_view_cpp.inja"; }
    json fn_header(const std::string& rule) const override {
        return {{"name", rule + "_read"}, {"kpfx", rule + "_v"}};   // CamelCase as-is
    }
    // The view nests the index at RUNTIME (it keeps the begin_struct/end_struct boundary
    // stencils — its post_lower is the base no-op) and decodes-on-access from the
    // just-built index, so it overrides the owning spelling: field refs / path-nav go
    // through the builder, builtins run against the `bbq::reader` (`r`), and the optional
    // value IS the child node (no `.value` — the view records the bare leaf).
    std::string field_ref(const std::string& name) const override {
        return "bbq_view_i64(r, \"" + name + "\")";
    }
    std::string field_ref_bytes(const std::string& name) const override {
        return "bbq_view_bytes(r, \"" + name + "\")";
    }
    std::string path_start(const std::string& name) const override {
        return "r.builder.find_field_str(\"" + name + "\")";
    }
    std::string path_field(const std::string& base, const std::string& field) const override {
        return "r.builder.find_child_str(" + base + ", \"" + field + "\")";
    }
    std::string path_index(const std::string& base, const std::string& idx) const override {
        return "r.builder.find_child_at(" + base + ", (int)(" + idx + "))";
    }
    std::string path_value(const std::string& expr) const override {
        return "bbq::node_int(" + expr + ", r.data)";
    }
    std::string cross_ref(const std::string&, int, const std::string& path) const override {
        return cross_ref_via_path(*this, path);
    }
    std::string builtin(const std::string& which) const override {
        if (which == "pos")        return "r.pos";
        if (which == "remaining")  return "r.remaining()";
        if (which == "at_end")     return "r.at_end()";
        if (which == "loop_index") return "r.loop_index()";
        if (which == "end")        return "r.effective_end()";
        if (which == "start")      return "r.builder.current_scope_start()";
        if (which == "peek")       return "(r.pos < r.effective_end() ? (int64_t)r.data[r.pos] : (int64_t)0)";
        if (which == "buffer")     return "bbq::bytes_view{ r.data + r.builder.current_scope_start(), (size_t)(r.pos - r.builder.current_scope_start()) }";
        return "/*?builtin*/";
    }
    void register_mappers(inja::Environment& env) const override {
        env.add_callback("prim_bytes",   1, [](inja::Arguments& a) { return prim_bytes_json(*a[0]); });
        env.add_callback("capture_expr", 1, [](inja::Arguments& a) { return capture_expr_json(*a[0]); });
    }
    void extra_data(json& data, const CompilerCtx&) const override {
        json externs = json::array();
        std::set<std::string> seen;
        for (auto& fnj : data["functions"])
            for (auto& o : fnj["ops"])
                if (o.value("op", std::string()) == "extern") {
                    std::string f = o["func"];
                    if (seen.insert(f).second) externs.push_back(f);
                }
        data["externs"]   = externs;
        data["namespace"] = ns_;
        data["has_ns"]    = !ns_.empty();
    }
};

// C++ ZCow WRITER: the dual of ZCowReader over the SAME burg lowering — walks the graph the
// reader emitted (FieldCapture index + CoW) and streams bytes via bbq::writer, enforcing the
// grammar. NO fold (boundaries stay runtime stencils, like the reader; the cursor navigates
// the graph at runtime); post_lower is `derive_write_prefixes` only (the count/@rest/pad
// linkage), the shared base method. Structure-driven, so the only spelled expr is
// set_endian's — field refs read the graph node, builtins are inert.
struct ZCowWriter : Emitter {
    std::string ns_, dir_;
    ZCowWriter(std::string ns, std::string dir) : ns_(std::move(ns)), dir_(std::move(dir)) {}
    std::string template_path() const override { return dir_ + "/writer_view_cpp.inja"; }
    json fn_header(const std::string& rule) const override {
        return {{"name", rule + "_write"}, {"kpfx", rule}};   // CamelCase; stencils {rule}_w{id}
    }
    // post_lower = derive_write_prefixes ONLY (no fold): it tags the count-field↔array
    // linkage (count_reserve/count_patch + count_id) the enforcement stencils read.
    void post_lower(json& functions) const override { derive_write_prefixes(functions); }
    // The enforcement template spells no exprs and uses no atom mappers (it navigates +
    // sets counts by literal name, then emit() serializes) — so no spelling overrides /
    // mappers are needed; only the namespace is injected.
    void register_mappers(inja::Environment&) const override {}
    void extra_data(json& data, const CompilerCtx&) const override {
        data["namespace"] = ns_; data["has_ns"] = !ns_.empty();
    }
};

// c-lite (C view) reader: the C emission of the SAME view lowering ZCowReader rides —
// advances the shared cursor (bbq_read.h) and records [start,end)+type spans into a
// bbq_capture_builder (bbq_lite.h), decode-on-access. It mixes OwningCReader's C naming
// (snake+prefix, the `kfn` sub-rule mapper, musttail stencils) with ZCowReader's view
// post_lower (the base no-op — boundaries stay runtime stencils, no owning fold) and a
// C-spelled view runtime. Read-only: no writer, no types header (cross_ref is NOT
// overridden — like ZCowReader, unused for the fixture; same denominator as the cpp view).
struct ViewCReader : Emitter {
    std::string tp_, dir_;
    ViewCReader(std::string tp, std::string dir) : tp_(std::move(tp)), dir_(std::move(dir)) {}
    std::string type_prefix() const override { return tp_; }
    std::string template_path() const override { return dir_ + "/reader_view_c.inja"; }
    json fn_header(const std::string& rule) const override {
        std::string base = cname(rule, tp_);
        return {{"name", base + "_read"}, {"kpfx", base + "_v"}};   // stencils <base>_v_k<id>
    }
    std::string decl(const std::string& rule) const override {
        return "bbq_capture_metadata " + cname(rule, tp_) +
               "_read(const uint8_t* data, size_t len, bbq_arena* arena);";
    }
    void register_mappers(inja::Environment& env) const override {
        env.add_callback("prim_bytes",   1, [](inja::Arguments& a) { return prim_bytes_json(*a[0]); });
        env.add_callback("capture_expr", 1, [](inja::Arguments& a) { return capture_expr_c_json(*a[0]); });
        std::string tp = tp_;
        env.add_callback("kfn", 1, [tp](inja::Arguments& a) { return cname(a[0]->get<std::string>(), tp); });
    }
    // ── C view spelling (decode-on-access over the builder/cursor; ctx = bbq_view_ctx_t*) ──
    std::string field_ref(const std::string& name) const override {
        return "bbq_view_i64(ctx, \"" + name + "\")";
    }
    std::string field_ref_bytes(const std::string& name) const override {
        return "bbq_view_bytes(ctx, \"" + name + "\")";
    }
    std::string path_start(const std::string& name) const override {
        return "bbq_cap_find_field_str(&ctx->builder, \"" + name + "\")";
    }
    std::string path_field(const std::string& base, const std::string& field) const override {
        return "bbq_cap_find_child_str(&ctx->builder, " + base + ", \"" + field + "\")";
    }
    std::string path_index(const std::string& base, const std::string& idx) const override {
        return "bbq_cap_find_child_at(&ctx->builder, " + base + ", (int)(" + idx + "))";
    }
    std::string path_value(const std::string& expr) const override {
        return "bbq_node_int(" + expr + ", ctx->cur.data)";
    }
    std::string cross_ref(const std::string&, int, const std::string& path) const override {
        return cross_ref_via_path(*this, path);
    }
    // C is an unfaithful host for the BBQ expression-type model: its relational/equality/
    // logical operators yield `int`, but the model (and C++) says `bool`. Cast those to
    // `_Bool` so a `compute`'s _Generic kind deduction matches the CEK (a `:bool` compute
    // records Bool, not Int). Other operators pass through unchanged.
    std::string bin_expr(const std::string& op, const std::string& l,
                         const std::string& r) const override {
        static const std::set<std::string> boolean =
            {"==", "!=", "<", ">", "<=", ">=", "&&", "||"};
        std::string e = "(" + l + " " + op + " " + r + ")";
        return boolean.count(op) ? "(_Bool)" + e : e;
    }
    std::string un_expr(const std::string& op, const std::string& x) const override {
        std::string e = "(" + op + x + ")";
        return op == "!" ? "(_Bool)" + e : e;
    }
    std::string builtin(const std::string& which) const override {
        if (which == "pos")        return "bbq_pos(&ctx->cur)";
        if (which == "remaining")  return "bbq_remaining(&ctx->cur)";
        if (which == "at_end")     return "bbq_at_end(&ctx->cur)";
        if (which == "loop_index") return "bbq_loop_index(&ctx->cur)";
        if (which == "end")        return "bbq_effective_end(&ctx->cur)";
        if (which == "start")      return "bbq_cap_current_scope_start(&ctx->builder)";
        if (which == "peek")       return "(bbq_pos(&ctx->cur) < bbq_effective_end(&ctx->cur) ? (int64_t)ctx->cur.data[bbq_pos(&ctx->cur)] : (int64_t)0)";
        if (which == "buffer")     return "((bbq_bytes_t){ctx->cur.data + bbq_cap_current_scope_start(&ctx->builder), bbq_pos(&ctx->cur) - bbq_cap_current_scope_start(&ctx->builder)})";
        return "/*?builtin*/";
    }
    void extra_data(json& data, const CompilerCtx& ctx) const override {
        json externs = json::array();
        std::set<std::string> seen;
        for (auto& fnj : data["functions"])
            for (auto& o : fnj["ops"])
                if (o.value("op", std::string()) == "extern") {
                    std::string f = o["func"];
                    if (seen.insert(f).second) externs.push_back(f);
                }
        data["externs"] = externs;
        // The c-lite reader has no types header, so it carries the grammar's @header (e.g. a
        // shared #include) AND @source at the top — {{ user_source_header }} / {{ user_source }} —
        // before the rules that use them.
        std::string hdr, src;
        if (ctx.ast)
            for (auto* cb : ctx.ast->codes) {
                if (cb->section == BBQ::CodeSection::HeaderBlock) hdr += std::string(cb->code) + "\n";
                if (cb->section == BBQ::CodeSection::SourceBlock) src += std::string(cb->code) + "\n";
            }
        data["user_source_header"] = hdr;
        data["user_source"] = src;
    }
    std::string prologue() const override { return kMustTail; }
};
}  // namespace

// cpp-zcow (view) reader — one Emitter subclass over the shared render_emit loop.
// `templates_dir` is the template directory; the subclass owns its own filename.
std::string render_reader_view(const CompilerCtx& ctx, const std::string& templates_dir,
                               const std::string& ns) {
    return render_emit(ctx, ZCowReader(ns, templates_dir), lower_grammar(ctx));
}

// cpp-zcow (view) WRITER — the dual of render_reader_view; ZCowWriter over the same loop.
std::string render_writer_view(const CompilerCtx& ctx, const std::string& templates_dir,
                               const std::string& ns) {
    return render_emit(ctx, ZCowWriter(ns, templates_dir), lower_grammar(ctx));
}

// c-lite (C view) reader — the C emission of the view lowering; ViewCReader over the same
// loop. Reader only (no writer / no types header); C uses the name prefix, so no `ns`.
std::string render_reader_view_c(const CompilerCtx& ctx, const std::string& templates_dir) {
    return render_emit(ctx, ViewCReader(ctx.type_prefix(), templates_dir), lower_grammar(ctx));
}

// Public forward-declarations for every rule's c-lite read fn — `bbq_capture_metadata
// <pfx>_<rule>_read(const uint8_t*, size_t, bbq_arena*);` (the ViewCReader's own decl
// spelling), for the generated reader header.
std::string render_reader_view_c_decls(const CompilerCtx& ctx) {
    ViewCReader r(ctx.type_prefix(), "");
    CompiledGrammar* g = ctx.ir;
    std::string out;
    for (int i = 0; i < g->rule_count; i++) out += r.decl(g->rules[i].name) + "\n";
    return out;
}

std::string render_reader_decls(const CompilerCtx& ctx) {
    // @header user-code blocks go in the types header (testTypes.h), which
    // testReader.h includes — emitting them here too would redefine them. The decl
    // spelling is the OwningCReader's own (the reader twin of render_writer_decls).
    OwningCReader r(ctx.type_prefix(), "");
    CompiledGrammar* g = ctx.ir;
    std::string out;
    for (int i = 0; i < g->rule_count; i++) out += r.decl(g->rules[i].name) + "\n";
    return out;
}

}  // namespace bbq::render
