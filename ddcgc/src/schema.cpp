#include "schema.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace ddcgc {

static FieldFlag parse_flag(const std::string& s) {
    if (s == "sequence") return FieldFlag::Sequence;
    if (s == "optional") return FieldFlag::Optional;
    return FieldFlag::None;
}

std::optional<Schema> load_asdl_schema(const std::string& path,
                                       std::string& error_out) {
    std::ifstream f(path);
    if (!f) {
        error_out = "cannot open ASDL JSON: " + path;
        return std::nullopt;
    }

    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        error_out = "ASDL JSON parse error in " + path + ": " + e.what();
        return std::nullopt;
    }

    Schema out;

    // Aliases — flat string map at the top of the module.
    if (doc.contains("aliases") && doc["aliases"].is_object()) {
        for (auto it = doc["aliases"].begin(); it != doc["aliases"].end(); ++it) {
            out.aliases[it.key()] = it.value().get<std::string>();
        }
    }

    if (!doc.contains("definitions") || !doc["definitions"].is_array()) {
        // Empty schema (no definitions) is permitted; downstream callers
        // will surface "constructor not found" if a rule references it.
        return out;
    }

    // First pass: collect sum names so we can validate field types
    // (and so callers can iterate sum membership). asdl tags sums whose
    // constructors are all zero-arg with `is_enum: true` — propagate
    // that so the typechecker and backend can lower them as `enum class`
    // values rather than tagged-union pointers.
    for (const auto& def : doc["definitions"]) {
        if (def.value("type", "") == "sum") {
            std::string sname = def.value("name", "");
            out.sum_names.insert(sname);
            if (def.value("is_enum", false)) {
                out.enum_sums.insert(sname);
            }
        }
    }

    // Second pass: walk constructors and their fields.
    for (const auto& def : doc["definitions"]) {
        if (def.value("type", "") != "sum") continue;
        std::string sum_name = def.value("name", "");
        if (!def.contains("types") || !def["types"].is_array()) continue;

        std::vector<std::string> ctor_names;
        for (const auto& ctor : def["types"]) {
            std::string cname = ctor.value("name", "");
            if (cname.empty()) continue;
            ctor_names.push_back(cname);

            Constructor c;
            c.name = cname;
            c.sum_name = sum_name;

            if (ctor.contains("fields") && ctor["fields"].is_array()) {
                for (const auto& fld : ctor["fields"]) {
                    Field f;
                    f.name      = fld.value("name", "");
                    f.type_name = fld.value("type", "");
                    f.flag      = parse_flag(fld.value("flag", "none"));
                    f.is_schema = fld.value("is_schema", false);
                    c.fields.push_back(std::move(f));
                }
            }

            out.constructors[cname] = std::move(c);
        }
        out.sum_constructors[sum_name] = std::move(ctor_names);
    }

    // Third pass: register product types. asdl products are
    // equivalent to single-constructor sums where the type name is
    // the constructor name (e.g. `bitfield_entry = (interned name,
    // int width_bits)`). Without this pass, references like
    // `list<ir.bitfield_entry>` fail type lookup and fall back to
    // `auto`, producing invalid `std::vector<auto>` in the generated
    // code. We register products in `constructors` so cpp_type_for
    // finds them, plus in `product_names` so the backend can keep
    // their snake_case naming (asdl emitters render products as
    // `struct switch_case { ... }`, not `SwitchCase`).
    for (const auto& def : doc["definitions"]) {
        if (def.value("type", "") != "product") continue;
        std::string pname = def.value("name", "");
        if (pname.empty()) continue;
        out.product_names.insert(pname);

        Constructor c;
        c.name = pname;
        c.sum_name = pname;   // products are self-named (no parent sum)

        if (def.contains("fields") && def["fields"].is_array()) {
            for (const auto& fld : def["fields"]) {
                Field f;
                f.name      = fld.value("name", "");
                f.type_name = fld.value("type", "");
                f.flag      = parse_flag(fld.value("flag", "none"));
                f.is_schema = fld.value("is_schema", false);
                c.fields.push_back(std::move(f));
            }
        }
        out.constructors[pname] = std::move(c);
    }

    // Module name — asdl puts it under "name" at the top, but the JSON
    // we've seen so far (ParserKont.json) doesn't always emit it; fall
    // back to deriving from the first definition's parent if we ever
    // need to. For now, leave empty if not present.
    if (doc.contains("name") && doc["name"].is_string()) {
        out.module_name = doc["name"].get<std::string>();
    }

    return out;
}

} // namespace ddcgc
