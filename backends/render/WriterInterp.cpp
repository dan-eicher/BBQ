#include "WriterInterp.h"

#include <unordered_map>

#include "bbq_writer.h"

namespace bbq::render {

using nlohmann::json;

namespace {

// The count/@rest field's storage encoding, spelled from the op's recorded primitive —
// the interpreted form of the template's `{% if o.count_prim.enc == "uleb" %}` chain.
znode::Enc enc_of(const json& prim) {
    const std::string e = prim.is_object() ? prim.value("enc", std::string()) : std::string();
    if (e == "uleb") return znode::Enc::Uleb;
    if (e == "sleb") return znode::Enc::Sleb;
    return znode::Enc::Fixed;
}

const char* cstr(const json& o, const char* key) {
    auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get_ref<const std::string&>().c_str() : "";
}

// One rule's ops, indexed by id — the interpreter's equivalent of the per-rule family of
// `<Rule>_w<id>` stencils the template emits.
struct Fn {
    const json* fn = nullptr;
    std::unordered_map<int, const json*> ops;
};

struct Interp {
    std::unordered_map<std::string, Fn> rules;
    std::string err;

    explicit Interp(const json& functions) {
        for (const auto& f : functions) {
            Fn e; e.fn = &f;
            for (const auto& o : f.at("ops")) e.ops[o.at("id").get<int>()] = &o;
            rules.emplace(f.value("rule", std::string()), std::move(e));
        }
    }

    bool fail(const std::string& m) { if (err.empty()) err = m; return false; }

    // A rule's enforcement entry: the template's `<Rule>_w_entry`.
    bool run_rule(const std::string& rule, bbq::writer& w) {
        auto it = rules.find(rule);
        if (it == rules.end()) return fail("no writer ops for rule '" + rule + "'");
        return step(it->second, it->second.fn->at("entry").get<int>(), w);
    }

    // The op walk. Every tail call in the template is a `goto next` here; the three places
    // it recurses (a resync element body, an optional's inner chain, an invoked sub-rule)
    // recurse here too.
    bool step(const Fn& f, int id, bbq::writer& w) {
        for (;;) {
            auto oi = f.ops.find(id);
            if (oi == f.ops.end()) return fail("writer op id " + std::to_string(id) + " not found");
            const json& o = *oi->second;
            const std::string k = o.value("op", std::string());

            auto go = [&](const char* key) -> bool {
                auto n = o.find(key);
                if (n == o.end() || !n->is_number_integer())
                    return fail("writer op '" + k + "' has no '" + key + "'");
                id = n->get<int>();
                return true;
            };

            if (k == "read") {
                // Nail's dependent field: remember where it lives now, so the construct that
                // determines it can overwrite it before emit() serializes.
                if (o.contains("count_reserve")) w.mark_count(o.at("count_id").get<int>(), cstr(o, "target"));
                else if (o.contains("rest_reserve")) w.mark_rest(cstr(o, "target"));
                if (!go("next")) return false;
            }
            else if (k == "begin_struct") {
                w.enter_struct(cstr(o, "name"));
                if (!go("next")) return false;
            }
            else if (k == "end_struct") {
                w.leave();
                if (!go("next")) return false;
            }
            else if (k == "array_begin" || k == "array_begin_grow") {
                if (o.contains("count_patch"))
                    w.set_count(o.at("count_id").get<int>(), w.eff_count_of(cstr(o, "field")),
                                enc_of(o.at("count_prim")));
                int64_t n = w.begin_array(cstr(o, "field"));
                if (n == 0) { w.end_array(); if (!go("end")) return false; continue; }
                w.push_loop(n);
                if (!go("body")) return false;
            }
            else if (k == "array_next" || k == "array_next_grow" || k == "array_next_count") {
                if (w.loop_next()) { if (!go("body")) return false; continue; }
                w.pop_loop(); w.end_array();
                if (!go("end")) return false;
            }
            else if (k == "array_begin_resync") {
                // resync element bodies end in array_elem_done, so the loop is driven here
                // (one body call per effective element) rather than cycled through array_next.
                if (o.contains("count_patch"))
                    w.set_count(o.at("count_id").get<int>(), w.eff_count_of(cstr(o, "field")),
                                enc_of(o.at("count_prim")));
                int64_t n = w.begin_array(cstr(o, "field"));
                w.push_loop(n);
                auto body = o.find("body");
                if (body == o.end()) { w.pop_loop(); w.end_array(); return fail("array_begin_resync has no 'body'"); }
                for (int64_t i = 0; i < n; i++) {
                    if (!step(f, body->get<int>(), w)) { w.pop_loop(); w.end_array(); return false; }
                    w.loop_next();
                }
                w.pop_loop(); w.end_array();
                if (!go("end")) return false;
            }
            else if (k == "end_array") {
                if (!go("next")) return false;
            }
            else if (k == "pop_interval") {
                // @rest closes: measure the (edited) window and write its length back.
                if (o.contains("rest")) w.set_rest(enc_of(o.at("prim")));
                if (!go("next")) return false;
            }
            else if (k == "switch") {
                const int tag = w.variant_tag(cstr(o, "tag_target"));
                int target = -1;
                for (const auto& c : o.at("cases"))
                    if (c.at("ordinal").get<int>() == tag) { target = c.at("target").get<int>(); break; }
                if (target < 0) {
                    if (!o.value("has_default", false))
                        return fail("switch: no case for variant tag " + std::to_string(tag));
                    target = o.at("default_target").get<int>();
                }
                id = target;
            }
            else if (k == "optional") {
                if (!go(w.has(cstr(o, "target")) ? "present" : "absent")) return false;
            }
            else if (k == "begin_optional") {
                if (w.has(cstr(o, "target"))) {
                    auto inner = o.find("inner");
                    if (inner == o.end()) return fail("begin_optional has no 'inner'");
                    if (!step(f, inner->get<int>(), w)) return false;
                }
                if (!go("next")) return false;
            }
            else if (k == "optional_prim") {
                if (!go("next")) return false;
            }
            else if (k == "invoke") {
                if (!run_rule(o.value("fn", std::string()), w)) return false;
                if (!go("next")) return false;
            }
            else if (k == "choice") {
                // union/alternatives: the matched arm is the named variant child the index
                // recorded, so the arm is selected by presence, not by re-trying the parse.
                for (const auto& arm : o.at("arms")) {
                    const char* name = cstr(arm, "name");
                    if (!w.has(name)) continue;
                    w.enter_struct(name);
                    bool ok = run_rule(arm.value("fn", std::string()), w);
                    w.leave();
                    return ok;
                }
                return fail("choice: no arm present in the capture index");
            }
            else if (k == "end_optional" || k == "return" || k == "array_elem_done") {
                return true;
            }
            else {
                // compute/constraint/read_bytes/extern/bitfield/seek/push_interval/set_endian:
                // no dependent field to fix here (emit() serializes the value/span; a
                // non-@rest interval is read-only).
                if (!go("next")) return false;
            }
        }
    }
};

}  // namespace

std::vector<uint8_t> run_writer(const json& functions, const std::string& rule,
                                const bbq::FieldCapture* root, const uint8_t* buf, size_t len,
                                bbq::zcow* zc, std::string* error, const VerifyFn& verify) {
    // The op-list is our own lowering, so a missing key is a toolchain bug, not user input —
    // but this runs behind the Python C API, where an escaping exception is a hard crash.
    // Catch it here and report it as the failure it is.
    try {
        Interp in(functions);
        bbq::writer w(root, buf, len, zc);
        if (!in.run_rule(rule, w)) {
            if (error) *error = in.err.empty() ? std::string("writer walk failed") : in.err;
            return {};
        }
        if (w.error) {
            if (error) *error = w.error;
            return {};
        }
        std::vector<uint8_t> out = w.emit_bytes();
        // (PUT): put(A × C) ⊆ C. The walk keeps the DERIVED fields consistent; it cannot
        // know that the edit still selects the shape it just replayed. Ask `get`.
        if (verify) {
            std::string why;
            if (!verify(out.data(), out.size(), &why)) {
                if (error)
                    *error = "the edit does not produce a document this grammar accepts"
                             + (why.empty() ? std::string() : ": " + why);
                return {};
            }
        }
        return out;
    } catch (const std::exception& e) {
        if (error) *error = std::string("malformed writer op-list: ") + e.what();
        return {};
    }
}

std::vector<const bbq::FieldCapture*> derived_fields(
    const json& functions, const std::string& rule,
    const bbq::FieldCapture* root, const uint8_t* buf, size_t len) {
    try {
        Interp in(functions);
        bbq::zcow scratch;                 // thrown away — only the marks are wanted
        bbq::writer w(root, buf, len, &scratch);
        in.run_rule(rule, w);              // a partial walk still marks what it reached
        return w.derived;
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace bbq::render
