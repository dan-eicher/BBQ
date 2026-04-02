#include "type_registry.h"
#include "exceptions.h"

TypeRegistry::TypeRegistry() {
    types_["identifier"] = "std::string";
    types_["string"] = "std::string";
    types_["int"] = "int";
    types_["int64"] = "int64_t";
    types_["bool"] = "bool";
    types_["float"] = "float";
}

void TypeRegistry::set_lang_c() {
    types_["identifier"] = "const char*";
    types_["string"]     = "const char*";
}

void TypeRegistry::register_type(const std::string& asdl_type, const std::string& target_type) {
    types_[asdl_type] = target_type;
}

void TypeRegistry::register_alias(const std::string& asdl_type, const std::string& target_type) {
    types_[asdl_type] = target_type;
    aliases_.insert(asdl_type);
}

std::string TypeRegistry::get_type(const std::string& asdl_type) const {
    auto it = types_.find(asdl_type);
    if (it == types_.end()) {
        throw TemplateException("Unmapped type: " + asdl_type +
            ". Available types: identifier, string, int, bool, float, or user-defined types");
    }
    return it->second;
}

bool TypeRegistry::is_base_type(const std::string& type) const {
    // User-defined aliases are treated as base types
    if (aliases_.count(type) > 0) {
        return true;
    }
    auto it = types_.find(type);
    if (it == types_.end()) {
        throw TemplateException("Unknown type in base type check: " + type);
    }
    const std::string& mapped = it->second;
    return mapped == "bool" || mapped == "int" || mapped == "int64_t" || mapped == "float" || mapped == "std::string";
}

bool TypeRegistry::has_type(const std::string& type) const {
    return types_.count(type) > 0;
}
