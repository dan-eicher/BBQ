#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Asdl();
}

static bool is_letter(char c) {
    return (((c >= 97 && c <= 122) || (c >= 65 && c <= 90)) || (c == 95));
}

static bool is_digit(char c) {
    return (c >= 48 && c <= 57);
}

static bool is_ident_continue(char c) {
    return (is_letter(c) || is_digit(c));
}

void Parser::setup_skip() {
    set_whitespace([](char c) -> bool {
        return ((((c == 13) || (c == 10)) || (c == 9)) || (c == 32));
    });
    set_comments({{"//", "\n", false, false}, {"/*", "*/", false, false}});
}


bool Parser::ident(peg::Span& out) {
    const char* _start = pos();
    if (at_end() || !is_letter(peek())) return false;
    advance();
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            if (at_end() || !is_ident_continue(peek())) return false;
            advance();
            return true;
        }()) { restore(_m0); break; }
    }
    out.ptr = _start; out.len = static_cast<int>(pos() - _start);
    return true;
}

bool Parser::string_lit(peg::Span& out) {
    const char* _start = pos();
    if (!match("\"")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    if (!match("\\")) return false;
                    if (at_end()) return false;
                    advance();
                    return true;
                }()) {} else {
                restore(_m1);
                {
                    auto _m2 = save();
                    bool _ok2 = [&]() -> bool {
                        if (!match("\"")) return false;
                        return true;
                    }();
                    restore(_m2);
                    if (_ok2) return false;
                }
                if (at_end()) return false;
                advance();
                }
            }
            return true;
        }()) { restore(_m0); break; }
    }
    if (!match("\"")) return false;
    out.ptr = _start; out.len = static_cast<int>(pos() - _start);
    return true;
}

bool Parser::parse_Asdl() {
    peg::Span name_span;
       std::string name;
       std::vector<asdl_ast::TypeAlias*> aliases;
       std::vector<asdl_ast::Definition*> definitions;
    skip();
    if (!match("module")) return false;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
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
    peg::Span ctype_span; std::string name; std::string ctype;
    skip();
    if (!match("type")) return false;
    skip();
    if (!parse_TypId(name)) return false;
    skip();
    if (!match("=")) return false;
    skip();
    if (!string_lit(ctype_span)) return false;
    ctype = peg::unescape_string_lit(ctype_span);
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
            if (!match("attributes")) return false;
            {
                auto _m2 = save();
                bool _ok2 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m2);
                if (_ok2) return false;
            }
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
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_FieldDecl(f)) return false;
            fields.push_back(f);
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_FieldDecl(f)) return false;
                    fields.push_back(f);
                    return true;
                }()) { restore(_m1); break; }
            }
            return true;
        }()) restore(_m0);
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
    peg::Span span;
    if (!std::islower(peek())) return false;
    skip();
    if (!ident(span)) return false;
    name = span.to_string();
    return true;
}

bool Parser::parse_ConId(std::string& name) {
    peg::Span span;
    if (!std::isupper(peek())) return false;
    skip();
    if (!ident(span)) return false;
    name = span.to_string();
    return true;
}

bool Parser::parse_AnyId(std::string& name) {
    peg::Span span;
    skip();
    if (!ident(span)) return false;
    name = span.to_string();
    return true;
}


