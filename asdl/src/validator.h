#ifndef ASDL_VALIDATOR_H
#define ASDL_VALIDATOR_H

#include <string>
#include <nlohmann/json.hpp>
#include "type_registry.h"

using json = nlohmann::json;

void validate_file_exists(const std::string& filename);
void validate_template_file(const std::string& filename);
void validate_output_path(const std::string& outfile);
void validate_json_structure(const json& module);
void validate_type_references(const json& module, const TypeRegistry& types);

#endif // ASDL_VALIDATOR_H
