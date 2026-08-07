#include "generator.h"
#include "exceptions.h"
#include "validator.h"
#include "string_utils.h"
#include "Parser.h"
#include <fstream>
#include <sstream>
#include <unordered_set>

Generator::Generator() {
    env_.set_trim_blocks(true);
    env_.set_lstrip_blocks(true);
    setup_callbacks();
}

void Generator::setup_callbacks() {
    env_.add_callback("get_type", 1, [this](inja::Arguments& args) {
        std::string type = args.at(0)->get<std::string>();
        return types_.get_type(type);
    });

    env_.add_callback("camelcase", 1, [](inja::Arguments& args) {
        try {
            return CamelCase(args.at(0)->get<std::string>());
        } catch (const std::exception& e) {
            throw TemplateException("Error in camelcase conversion: " + std::string(e.what()));
        }
    });

    env_.add_callback("snakecase", 1, [](inja::Arguments& args) {
        try {
            return SnakeCase(args.at(0)->get<std::string>());
        } catch (const std::exception& e) {
            throw TemplateException("Error in snakecase conversion: " + std::string(e.what()));
        }
    });

    env_.add_callback("not_base_type", 1, [this](inja::Arguments& args) {
        std::string type_name = args.at(0)->get<std::string>();
        return !types_.is_base_type(type_name);
    });

    env_.add_callback("safe_name", 1, [](inja::Arguments& args) {
        std::string name = args.at(0)->get<std::string>();
        return get_safe_name(name);
    });

    env_.add_callback("concat", 2, [](inja::Arguments& args) {
        std::string a = args.at(0)->get<std::string>();
        std::string b = args.at(1)->get<std::string>();
        return a + b;
    });

    // C type name: module_name + "_" + def_name in snake_case
    // e.g., c_name("ast", "expr") → "ast_expr_t"
    env_.add_callback("c_name", 2, [](inja::Arguments& args) {
        std::string mod = SnakeCase(args.at(0)->get<std::string>());
        std::string def = SnakeCase(args.at(1)->get<std::string>());
        return mod + "_" + def + "_t";
    });
}

asdl_ast::Module* Generator::parse(const std::string& input_file) {
    std::ifstream file(input_file);
    if (!file.good()) {
        throw ParseException("Failed to open file: " + input_file);
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    Parser parser;
    parser.init(content.data(), static_cast<int>(content.size()));

    if (!parser.parse()) {
        std::stringstream msg;
        if (parser.has_errors()) {
            auto& errs = parser.errors();
            msg << "Parser error at line " << errs[0].line
                << ", col " << errs[0].col << ": " << errs[0].message;
        } else {
            auto fur = parser.furthest();
            msg << "Parse failed near line " << fur.line << ", col " << fur.col;
        }
        throw ParseException(msg.str());
    }

    if (!parser.ast) {
        throw ParseException("Parser returned null AST");
    }

    return parser.ast;
}

static std::string flag_to_string(asdl_ast::Flag f) {
    switch (f) {
        case asdl_ast::Flag::Optional: return "optional";
        case asdl_ast::Flag::Sequence: return "sequence";
        default: return "none";
    }
}

static json fields_to_json(const std::vector<asdl_ast::Field*>& fields) {
    json arr = json::array();
    for (auto* f : fields) {
        arr.push_back({
            {"type", f->type_name},
            {"flag", flag_to_string(f->flag)},
            {"name", f->name}
        });
    }
    return arr;
}

json Generator::to_json(const asdl_ast::Module* module) {
    json result;
    result["name"] = module->name;
    result["definitions"] = json::array();
    result["aliases"] = json::object();

    for (auto* alias : module->aliases) {
        result["aliases"][alias->name] = alias->ctype;
    }

    for (auto* def : module->definitions) {
        json jdef;
        jdef["name"] = def->name;

        if (auto* prod = dynamic_cast<asdl_ast::Product*>(def->body)) {
            jdef["type"] = "product";
            jdef["fields"] = fields_to_json(prod->fields);
        } else if (auto* sum = dynamic_cast<asdl_ast::Sum*>(def->body)) {
            jdef["type"] = "sum";
            jdef["types"] = json::array();
            for (auto* ctor : sum->types) {
                jdef["types"].push_back({
                    {"name", ctor->name},
                    {"fields", fields_to_json(ctor->fields)}
                });
            }
            jdef["attributes"] = fields_to_json(sum->attributes);
        }

        result["definitions"].push_back(jdef);
    }

    return result;
}

void Generator::process(json& module) {
    module["bases"] = json::array();

    // Register type aliases first (these are treated as base types)
    if (module.contains("aliases") && module["aliases"].is_object()) {
        for (auto& [name, ctype] : module["aliases"].items()) {
            types_.register_alias(name, ctype.get<std::string>());
        }
    }

    std::unordered_set<std::string> defined_types;
    for (const json& def : module["definitions"]) {
        defined_types.insert(def["name"].get<std::string>());
    }

    // Tag each field with is_schema = whether its type is a sum/product
    // defined in this module (vs primitive or alias). Lets the template
    // emit generic arity/child accessors without a sidecar dispatch.
    auto tag_fields = [&](json& fields) {
        for (json& f : fields) {
            std::string t = f["type"].get<std::string>();
            f["is_schema"] = defined_types.count(t) > 0;
        }
    };
    for (json& def : module["definitions"]) {
        if (def["type"] == "sum" && def.contains("types")) {
            for (json& ctor : def["types"]) {
                if (ctor.contains("fields")) tag_fields(ctor["fields"]);
            }
        } else if (def["type"] == "product" && def.contains("fields")) {
            tag_fields(def["fields"]);
        }
    }

    std::string mod = module["name"].get<std::string>();

    for (json& def : module["definitions"]) {
        std::string def_name = def["name"].get<std::string>();
        std::string c_type = SnakeCase(mod) + "_" + SnakeCase(def_name) + "_t";

        if (def["type"] == "sum") {
            // Detect enum-like sum types: all constructors have zero fields
            bool is_enum = true;
            for (const auto& ctor : def["types"]) {
                if (!ctor["fields"].empty()) {
                    is_enum = false;
                    break;
                }
            }
            def["is_enum"] = is_enum;

            if (types_.is_c_mode()) {
                if (is_enum) {
                    types_.register_type(def_name, c_type);
                } else {
                    types_.register_type(def_name, c_type + "*");
                    module["bases"].push_back(c_type);
                }
            } else if (types_.is_java_mode()) {
                // Java has no enums before 1.5, so a field-free sum is a domain
                // of int constants and its fields are plain ints.
                //
                // A single-constructor sum is one final class named for its
                // constructor: there is no tag to switch on, and an abstract
                // base would collide with the constructor whenever the two
                // names camel-case alike (`unit = Unit(...)` is the common
                // shape). A multi-constructor sum is an abstract base plus one
                // final subclass each, carrying a `kind` tag.
                if (is_enum) {
                    types_.register_type(def_name, "int");
                } else if (def["types"].size() == 1) {
                    std::string only = def["types"][0]["name"];
                    types_.register_type(def_name, only);
                    module["bases"].push_back(only);
                } else {
                    std::string base = CamelCase(def_name);
                    types_.register_type(def_name, base);
                    module["bases"].push_back(base);
                    for (json& value : def["types"]) {
                        value["base"] = base;
                    }
                }
            } else {
                if (is_enum) {
                    std::string name = CamelCase(def_name);
                    types_.register_type(def_name, name);
                } else if (def["types"].size() == 1) {
                    std::string name = def["types"][0]["name"];
                    types_.register_type(def_name, name + "*");
                    module["bases"].push_back(name);
                    for (json& value : def["types"]) {
                        value["base"] = "ASTNode";
                    }
                } else {
                    std::string name = CamelCase(def_name);
                    types_.register_type(def_name, name + "*");
                    module["bases"].push_back(name);
                    for (json& value : def["types"]) {
                        value["base"] = name;
                    }
                }
            }
        } else {
            def["is_enum"] = false;
            if (types_.is_c_mode()) {
                types_.register_type(def_name, c_type);
            } else if (types_.is_java_mode()) {
                types_.register_type(def_name, CamelCase(def_name));
            } else {
                types_.register_type(def_name, def_name);
            }
        }
    }

    validate_type_references(module, types_);
}

void Generator::generate(const json& module, const std::string& template_file, std::ostream& output) {
    inja::Template temp;
    try {
        temp = env_.parse_template(template_file);
    } catch (const std::exception& e) {
        throw TemplateException("Failed to parse template file '" + template_file + "': " + e.what());
    }

    try {
        output << env_.render(temp, module);
    } catch (const std::exception& e) {
        throw TemplateException(std::string("Failed to render template: ") + e.what());
    }
}

void Generator::generate(const json& module, const std::string& template_file, const std::string& output_file) {
    inja::Template temp;
    try {
        temp = env_.parse_template(template_file);
    } catch (const std::exception& e) {
        throw TemplateException("Failed to parse template file '" + template_file + "': " + e.what());
    }

    try {
        env_.write(temp, module, output_file);
    } catch (const std::exception& e) {
        throw TemplateException(std::string("Failed to render template: ") + e.what());
    }
}
