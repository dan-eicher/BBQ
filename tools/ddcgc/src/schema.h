// ddcgc — schema loader for asdl JSON sidecars
//
// Consumes the JSON output produced by `asdl --json`. Same file
// format burgc consumes; the loader here tracks every field's name,
// type, and cardinality flag — typecheck needs all of it to validate
// rule-pattern field binds and call-arg types.

#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ddcgc {

// Type cardinality flag from asdl: bare type ('none'), sequence ('*' suffix),
// or optional ('?' suffix).
enum class FieldFlag { None, Sequence, Optional };

// A single field in a constructor: `name: type` with cardinality.
struct Field {
    std::string name;
    std::string type_name;   // asdl type — "kont", "string", "int", or alias name
    FieldFlag   flag = FieldFlag::None;
};

// A constructor case within a sum type.
struct Constructor {
    std::string name;        // e.g. "KSeq"
    std::string sum_name;    // parent sum-type name, e.g. "kont"
    std::vector<Field> fields;
};

// A loaded asdl schema (one .json file).
struct Schema {
    std::string module_name;

    // Type aliases declared at the top of the .asdl module
    // (e.g. `type ident_t = "std::string"`). Maps alias → underlying.
    std::map<std::string, std::string> aliases;

    // Set of all sum-type names declared in this module.
    std::set<std::string> sum_names;

    // For each sum, the list of constructor names belonging to it.
    std::map<std::string, std::vector<std::string>> sum_constructors;

    // For each constructor, the full field list.
    std::map<std::string, Constructor> constructors;
};

// Load an asdl JSON sidecar. Returns nullopt on parse / IO error.
// On error, `error_out` is filled with a human-readable message.
std::optional<Schema> load_asdl_schema(const std::string& path,
                                       std::string& error_out);

} // namespace ddcgc
