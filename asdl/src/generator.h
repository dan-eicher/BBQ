#ifndef ASDL_GENERATOR_H
#define ASDL_GENERATOR_H

#include <string>
#include <ostream>
#include <nlohmann/json.hpp>
#include <inja.hpp>
#include "type_registry.h"
#include "AsdlAST.h"

using json = nlohmann::json;

class Generator {
public:
    Generator();

    asdl_ast::Module* parse(const std::string& input_file);
    json to_json(const asdl_ast::Module* module);
    void process(json& module);
    void generate(const json& module, const std::string& template_file, std::ostream& output);
    void generate(const json& module, const std::string& template_file, const std::string& output_file);

    TypeRegistry& types() { return types_; }
    const TypeRegistry& types() const { return types_; }

private:
    TypeRegistry types_;
    inja::Environment env_;

    void setup_callbacks();
};

#endif // ASDL_GENERATOR_H
