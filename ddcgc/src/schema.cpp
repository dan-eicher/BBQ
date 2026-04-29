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
    // (and so callers can iterate sum membership).
    for (const auto& def : doc["definitions"]) {
        if (def.value("type", "") == "sum") {
            out.sum_names.insert(def.value("name", ""));
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
                    c.fields.push_back(std::move(f));
                }
            }

            out.constructors[cname] = std::move(c);
        }
        out.sum_constructors[sum_name] = std::move(ctor_names);
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
