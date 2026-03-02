#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Asdl();
}

void Parser::setup_skip() {
    set_whitespace([](char c) -> bool {
        return ((((c == 13) || (c == 10)) || (c == 9)) || (c == 32));
    });
    set_comments({{"//", "\n", false}, {"/*", "*/", false}});
}


bool Parser::parse_Asdl() {
    std::string name;
       std::vector<asdl_ast::TypeAlias*> aliases;
       std::vector<asdl_ast::Definition*> definitions;
    skip();
    if (!keyword("module")) return false;
    skip();
    if (!ident(name)) return false;
    skip();
    if (!match("{")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!parse_TypeAliasDecl(aliases)) return false;
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!parse_DefinitionDecl(definitions)) return false;
                }
            }
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match("}")) return false;
    ast = new asdl_ast::Module(std::move(name),
           std::move(aliases), std::move(definitions));
    return true;
}

bool Parser::parse_TypeAliasDecl(std::vector<asdl_ast::TypeAlias*>& aliases) {
    std::string name; std::string ctype;
    skip();
    if (!keyword("type")) return false;
    skip();
    if (!parse_TypId(name)) return false;
    skip();
    if (!match("=")) return false;
    skip();
    if (!string_lit(ctype)) return false;
    aliases.push_back(new asdl_ast::TypeAlias(std::move(name), std::move(ctype)));
    return true;
}

bool Parser::parse_DefinitionDecl(std::vector<asdl_ast::Definition*>& defs) {
    std::string name; asdl_ast::TypeBody* body;
    skip();
    if (!parse_TypId(name)) return false;
    skip();
    if (!match("=")) return false;
    skip();
    if (!parse_TypeBodyDecl(body)) return false;
    defs.push_back(new asdl_ast::Definition(std::move(name), body));
    return true;
}

bool Parser::parse_TypeBodyDecl(asdl_ast::TypeBody*& result) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_ProductBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_SumBody(result)) return false;
        }
    }
    return true;
}

bool Parser::parse_ProductBody(asdl_ast::TypeBody*& result) {
    std::vector<asdl_ast::Field*> fields;
    skip();
    if (!parse_FieldList(fields)) return false;
    result = new asdl_ast::Product(std::move(fields));
    return true;
}

bool Parser::parse_SumBody(asdl_ast::TypeBody*& result) {
    std::vector<asdl_ast::Constructor*> types;
       std::vector<asdl_ast::Field*> attributes;
       asdl_ast::Constructor* ctor;
    skip();
    if (!parse_ConstructorDecl(ctor)) return false;
    types.push_back(ctor);
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("|")) return false;
            skip();
            if (!parse_ConstructorDecl(ctor)) return false;
            types.push_back(ctor);
            return true;
        }()) { restore(_m0); break; }
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!keyword("attributes")) return false;
            skip();
            if (!parse_FieldList(attributes)) return false;
            return true;
        }()) restore(_m1);
    }
    result = new asdl_ast::Sum(std::move(types), std::move(attributes));
    return true;
}

bool Parser::parse_ConstructorDecl(asdl_ast::Constructor*& result) {
    std::string name;
       std::vector<asdl_ast::Field*> fields;
    skip();
    if (!parse_ConId(name)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_FieldList(fields)) return false;
            return true;
        }()) restore(_m0);
    }
    result = new asdl_ast::Constructor(std::move(name), std::move(fields));
    return true;
}

bool Parser::parse_FieldList(std::vector<asdl_ast::Field*>& fields) {
    asdl_ast::Field* f;
    skip();
    if (!match("(")) return false;
    skip();
    if (!parse_FieldDecl(f)) return false;
    fields.push_back(f);
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match(",")) return false;
            skip();
            if (!parse_FieldDecl(f)) return false;
            fields.push_back(f);
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match(")")) return false;
    return true;
}

bool Parser::parse_FieldDecl(asdl_ast::Field*& result) {
    std::string type_name; std::string name;
       asdl_ast::Flag flag = asdl_ast::Flag::None;
    skip();
    if (!parse_TypId(type_name)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("?")) return false;
            flag = asdl_ast::Flag::Optional;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("*")) return false;
            flag = asdl_ast::Flag::Sequence;
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!parse_AnyId(name)) return false;
    result = new asdl_ast::Field(std::move(type_name), flag, std::move(name));
    return true;
}

bool Parser::parse_TypId(std::string& name) {
    if (!std::islower(peek())) return false;
    skip();
    if (!ident(name)) return false;
    return true;
}

bool Parser::parse_ConId(std::string& name) {
    if (!std::isupper(peek())) return false;
    skip();
    if (!ident(name)) return false;
    return true;
}

bool Parser::parse_AnyId(std::string& name) {
    skip();
    if (!ident(name)) return false;
    return true;
}


