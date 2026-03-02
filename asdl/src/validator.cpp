#include "validator.h"
#include "exceptions.h"
#include <fstream>
#include <iostream>
#include <vector>

void validate_file_exists(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.good()) {
        throw FileException(filename, "open");
    }
}

void validate_template_file(const std::string& filename) {
    validate_file_exists(filename);

    std::ifstream file(filename);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::vector<std::string> required_placeholders = {"{{name}}", "{% for def in definitions %}"};
    for (const auto& placeholder : required_placeholders) {
        if (content.find(placeholder) == std::string::npos) {
            std::cerr << "Warning: Template file may be missing required placeholder: " << placeholder << std::endl;
        }
    }
}

void validate_output_path(const std::string& outfile) {
    if (outfile.empty()) return;

    std::ofstream test_file(outfile, std::ios::app);
    if (!test_file.good()) {
        throw FileException(outfile, "create/write to");
    }
    test_file.close();
}

void validate_json_structure(const json& module) {
    if (!module.contains("name")) {
        throw ParseException("Module is missing 'name' field");
    }

    if (!module.contains("definitions")) {
        throw ParseException("Module is missing 'definitions' field");
    }

    if (!module["definitions"].is_array()) {
        throw ParseException("Module 'definitions' field must be an array");
    }

    for (size_t i = 0; i < module["definitions"].size(); ++i) {
        const auto& def = module["definitions"][i];

        if (!def.contains("name")) {
            throw ParseException("Definition " + std::to_string(i) + " is missing 'name' field");
        }

        if (!def.contains("type")) {
            throw ParseException("Definition '" + def["name"].get<std::string>() + "' is missing 'type' field");
        }

        std::string type = def["type"];
        if (type != "product" && type != "sum") {
            throw ParseException("Definition '" + def["name"].get<std::string>() +
                "' has invalid type: " + type + " (must be 'product' or 'sum')");
        }

        if (type == "sum") {
            if (!def.contains("types") || !def["types"].is_array()) {
                throw ParseException("Sum type '" + def["name"].get<std::string>() + "' must have 'types' array");
            }

            if (def["types"].empty()) {
                throw ParseException("Sum type '" + def["name"].get<std::string>() + "' cannot have empty 'types' array");
            }
        }

        if (type == "product") {
            if (!def.contains("fields") || !def["fields"].is_array()) {
                throw ParseException("Product type '" + def["name"].get<std::string>() + "' must have 'fields' array");
            }
        }
    }
}

void validate_type_references(const json& module, const TypeRegistry& types) {
    auto validate_fields = [&](const json& fields) {
        for (const auto& field : fields) {
            if (field.contains("type")) {
                std::string field_type = field["type"];
                if (!types.has_type(field_type)) {
                    std::cerr << "Warning: Field '" << field["name"] << "' references unknown type: " << field_type << std::endl;
                }
            }
        }
    };

    for (const json& def : module["definitions"]) {
        if (def.contains("fields")) {
            validate_fields(def["fields"]);
        }

        if (def.contains("types")) {
            for (const auto& type : def["types"]) {
                if (type.contains("fields")) {
                    validate_fields(type["fields"]);
                }
            }
        }
    }
}
