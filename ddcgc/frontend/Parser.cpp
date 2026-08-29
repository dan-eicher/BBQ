#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Ddcg();
}

static bool is_letter(char c) {
    return ((((unsigned char)c >= 97 && (unsigned char)c <= 122) || ((unsigned char)c >= 65 && (unsigned char)c <= 90)) || ((unsigned char)c == 95));
}

static bool is_digit(char c) {
    return ((unsigned char)c >= 48 && (unsigned char)c <= 57);
}

static bool is_ident_continue(char c) {
    return (is_letter(c) || is_digit(c));
}

void Parser::setup_skip() {
    set_whitespace([](char c) -> bool {
        return (((((unsigned char)c == 13) || ((unsigned char)c == 10)) || ((unsigned char)c == 9)) || ((unsigned char)c == 32));
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

bool Parser::integer(peg::Span& out) {
    const char* _start = pos();
    if (at_end() || !is_digit(peek())) return false;
    advance();
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            if (at_end() || !is_digit(peek())) return false;
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

bool Parser::parse_Ddcg() {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::Header*> headers;
       std::vector<DdcgAst::Import*> imports;
       std::vector<DdcgAst::DestDecl*> destinations;
       DdcgAst::TypeRef* ir_root = nullptr;
       std::vector<DdcgAst::Aux*> auxiliaries;
       std::vector<DdcgAst::Pred*> predicates;
       std::vector<DdcgAst::Fun*> funs;
       std::vector<DdcgAst::LibraryImport*> libraries;
       std::string members;
       std::string priv;
       std::vector<std::string> desugared;
       std::vector<DdcgAst::Rule*> rules;
       bool internal_dispatchers = false;
    skip();
    if (!match("COMPILER")) return false;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("INTERNAL_DISPATCHERS")) return false;
            internal_dispatchers = true;
            return true;
        }()) restore(_m0);
    }
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_HeaderBlock(headers)) return false;
            return true;
        }()) { restore(_m1); break; }
    }
    skip();
    if (!match("IMPORTS")) return false;
    for (;;) {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ImportDecl(imports)) return false;
            return true;
        }()) { restore(_m2); break; }
    }
    skip();
    if (!match("DESTINATIONS")) return false;
    for (;;) {
        auto _m3 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_DestEntry(destinations, ir_root)) return false;
            return true;
        }()) { restore(_m3); break; }
    }
    skip();
    if (!match("AUXILIARIES")) return false;
    for (;;) {
        auto _m4 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_AuxiliaryDecl(auxiliaries)) return false;
            return true;
        }()) { restore(_m4); break; }
    }
    {
        auto _m5 = save();
        if (![&]() -> bool {
            skip();
            if (!match("PREDICATES")) return false;
            for (;;) {
                auto _m6 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_PredicateDecl(predicates)) return false;
                    return true;
                }()) { restore(_m6); break; }
            }
            return true;
        }()) restore(_m5);
    }
    {
        auto _m7 = save();
        if (![&]() -> bool {
            skip();
            if (!match("MEMBERS")) return false;
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", members)) return false;
            return true;
        }()) restore(_m7);
    }
    {
        auto _m8 = save();
        if (![&]() -> bool {
            skip();
            if (!match("PRIVATE")) return false;
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", priv)) return false;
            return true;
        }()) restore(_m8);
    }
    {
        auto _m9 = save();
        if (![&]() -> bool {
            skip();
            if (!match("DESUGARED")) return false;
            skip();
            if (!ident(name_span)) return false;
            desugared.push_back(name_span.to_string());
            for (;;) {
                auto _m10 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!ident(name_span)) return false;
                    desugared.push_back(name_span.to_string());
                    return true;
                }()) { restore(_m10); break; }
            }
            return true;
        }()) restore(_m9);
    }
    for (;;) {
        auto _m11 = save();
        if (![&]() -> bool {
            {
                auto _m12 = save();
                if ([&]() -> bool {
                    skip();
                    if (!parse_FunDecl(funs)) return false;
                    return true;
                }()) {} else {
                restore(_m12);
                skip();
                if (!parse_LibraryImport(libraries)) return false;
                }
            }
            return true;
        }()) { restore(_m11); break; }
    }
    skip();
    if (!match("RULES")) return false;
    for (;;) {
        auto _m13 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_RuleDecl(rules)) return false;
            return true;
        }()) { restore(_m13); break; }
    }
    ast = new DdcgAst::File(std::move(name),
           std::move(headers), std::move(imports),
           std::move(destinations), ir_root,
           std::move(auxiliaries), std::move(predicates),
           std::move(funs), std::move(libraries),
           std::move(members), std::move(priv),
           std::move(desugared), std::move(rules),
           internal_dispatchers);
       ast->loc = DdcgAst::SourceLoc{nullptr, 1, 1};
    return true;
}

bool Parser::parse_HeaderBlock(std::vector<DdcgAst::Header*>& headers) {
    std::string code;
    skip();
    if (!match("(.")) return false;
    if (!scan_to("." ")", code)) return false;
    auto* h = new DdcgAst::Header(std::move(code));
         h->loc = Loc();
         headers.push_back(h);
    return true;
}

bool Parser::parse_ImportDecl(std::vector<DdcgAst::Import*>& imports) {
    peg::Span name_span; peg::Span path_span;
       std::string name; std::string path;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    skip();
    if (!match("=")) return false;
    skip();
    if (!string_lit(path_span)) return false;
    path = peg::unescape_string_lit(path_span);
    auto* i = new DdcgAst::Import(std::move(name), std::move(path));
         i->loc = Loc();
         imports.push_back(i);
    return true;
}

bool Parser::parse_DestEntry(std::vector<DdcgAst::DestDecl*>& dests, DdcgAst::TypeRef*& ir_root) {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::DestVariant*> variants;
       std::vector<DdcgAst::DestField*> fields;
       DdcgAst::TypeRef* ty = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("data_dest")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            skip();
            if (!match("{")) return false;
            skip();
            if (!parse_VariantList(variants)) return false;
            skip();
            if (!match("}")) return false;
            auto* d = new DdcgAst::DataDest(std::move(name), std::move(variants));
         d->loc = Loc();
         dests.push_back(d);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("ctrl_dest")) return false;
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
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            skip();
            if (!match("{")) return false;
            skip();
            if (!parse_VariantList(variants)) return false;
            skip();
            if (!match("}")) return false;
            auto* d = new DdcgAst::CtrlDest(std::move(name), std::move(variants));
         d->loc = Loc();
         dests.push_back(d);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("env_dest")) return false;
            {
                auto _m3 = save();
                bool _ok3 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m3);
                if (_ok3) return false;
            }
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            skip();
            if (!match("{")) return false;
            skip();
            if (!parse_VariantList(variants)) return false;
            skip();
            if (!match("}")) return false;
            auto* d = new DdcgAst::EnvDest(std::move(name), std::move(variants));
         d->loc = Loc();
         dests.push_back(d);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("sum")) return false;
            {
                auto _m4 = save();
                bool _ok4 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m4);
                if (_ok4) return false;
            }
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            skip();
            if (!match("{")) return false;
            skip();
            if (!parse_VariantList(variants)) return false;
            skip();
            if (!match("}")) return false;
            auto* d = new DdcgAst::Sum(std::move(name), std::move(variants));
         d->loc = Loc();
         dests.push_back(d);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("ir_root")) return false;
        {
            auto _m5 = save();
            bool _ok5 = [&]() -> bool {
                if (at_end() || !is_ident_continue(peek())) return false;
                advance();
                return true;
            }();
            restore(_m5);
            if (_ok5) return false;
        }
        skip();
        if (!parse_TypeRef_(&ty)) return false;
        ir_root = ty;
        }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_VariantList(std::vector<DdcgAst::DestVariant*>& variants) {
    DdcgAst::DestVariant* v;
    skip();
    if (!parse_DestVariant_(&v)) return false;
    variants.push_back(v);
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("|")) return false;
            skip();
            if (!parse_DestVariant_(&v)) return false;
            variants.push_back(v);
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_DestVariant_(DdcgAst::DestVariant* * result) {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::DestField*> fields;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_DestFieldDecl(fields)) return false;
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_DestFieldDecl(fields)) return false;
                    return true;
                }()) { restore(_m1); break; }
            }
            skip();
            if (!match(")")) return false;
            return true;
        }()) restore(_m0);
    }
    auto* v = new DdcgAst::DestVariant(std::move(name), std::move(fields));
         v->loc = Loc();
         *result = v;
    return true;
}

bool Parser::parse_DestFieldDecl(std::vector<DdcgAst::DestField*>& fields) {
    peg::Span name_span; std::string name;
       DdcgAst::TypeRef* ty = nullptr;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TypeRef_(&ty)) return false;
    auto* f = new DdcgAst::DestField(std::move(name), ty);
         f->loc = Loc();
         fields.push_back(f);
    return true;
}

bool Parser::parse_AuxiliaryDecl(std::vector<DdcgAst::Aux*>& auxiliaries) {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::TypeRef*> params;
       DdcgAst::TypeRef* ret = nullptr;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    skip();
    if (!match(":")) return false;
    skip();
    if (!match("(")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_TypeRefList(params)) return false;
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match(")")) return false;
    skip();
    if (!match("->")) return false;
    skip();
    if (!parse_TypeRef_(&ret)) return false;
    auto* a = new DdcgAst::Aux(std::move(name), std::move(params), ret);
         a->loc = Loc();
         auxiliaries.push_back(a);
    return true;
}

bool Parser::parse_PredicateDecl(std::vector<DdcgAst::Pred*>& predicates) {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::TypeRef*> params;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    skip();
    if (!match(":")) return false;
    skip();
    if (!match("(")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_TypeRefList(params)) return false;
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match(")")) return false;
    skip();
    if (!match("->")) return false;
    skip();
    if (!match("bool")) return false;
    {
        auto _m1 = save();
        bool _ok1 = [&]() -> bool {
            if (at_end() || !is_ident_continue(peek())) return false;
            advance();
            return true;
        }();
        restore(_m1);
        if (_ok1) return false;
    }
    auto* p = new DdcgAst::Pred(std::move(name), std::move(params));
         p->loc = Loc();
         predicates.push_back(p);
    return true;
}

bool Parser::parse_FunDecl(std::vector<DdcgAst::Fun*>& funs) {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::FunParam*> params;
       DdcgAst::TypeRef* ret = nullptr;
       std::vector<DdcgAst::Stmt*> body;
    skip();
    if (!match("fun")) return false;
    {
        auto _m0 = save();
        bool _ok0 = [&]() -> bool {
            if (at_end() || !is_ident_continue(peek())) return false;
            advance();
            return true;
        }();
        restore(_m0);
        if (_ok0) return false;
    }
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    skip();
    if (!match("(")) return false;
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_FunParamDecl(params)) return false;
            for (;;) {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_FunParamDecl(params)) return false;
                    return true;
                }()) { restore(_m2); break; }
            }
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!match(")")) return false;
    skip();
    if (!match("->")) return false;
    skip();
    if (!parse_TypeRef_(&ret)) return false;
    skip();
    if (!parse_Block_(body)) return false;
    auto* f = new DdcgAst::Fun(std::move(name), std::move(params), ret, std::move(body));
         f->loc = Loc();
         funs.push_back(f);
    return true;
}

bool Parser::parse_FunParamDecl(std::vector<DdcgAst::FunParam*>& params) {
    peg::Span name_span; std::string name;
       DdcgAst::TypeRef* ty = nullptr;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TypeRef_(&ty)) return false;
    auto* p = new DdcgAst::FunParam(std::move(name), ty);
         p->loc = Loc();
         params.push_back(p);
    return true;
}

bool Parser::parse_LibraryImport(std::vector<DdcgAst::LibraryImport*>& libraries) {
    peg::Span path_span; std::string path;
    skip();
    if (!match("import")) return false;
    {
        auto _m0 = save();
        bool _ok0 = [&]() -> bool {
            if (at_end() || !is_ident_continue(peek())) return false;
            advance();
            return true;
        }();
        restore(_m0);
        if (_ok0) return false;
    }
    skip();
    if (!string_lit(path_span)) return false;
    path = peg::unescape_string_lit(path_span);
    auto* l = new DdcgAst::LibraryImport(std::move(path));
         l->loc = Loc();
         libraries.push_back(l);
    return true;
}

bool Parser::parse_TypeRefList(std::vector<DdcgAst::TypeRef*>& refs) {
    DdcgAst::TypeRef* t = nullptr;
    skip();
    if (!parse_TypeRef_(&t)) return false;
    refs.push_back(t);
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match(",")) return false;
            skip();
            if (!parse_TypeRef_(&t)) return false;
            refs.push_back(t);
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_TypeRef_(DdcgAst::TypeRef* * result) {
    peg::Span name_span; peg::Span sub_span;
       std::string mod; std::string name;
       std::vector<DdcgAst::TypeRef*> elems;
       DdcgAst::TypeRef* inner = nullptr;
       bool is_ptr = false;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("list")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!match("<")) return false;
            skip();
            if (!parse_TypeRef_(&inner)) return false;
            skip();
            if (!match(">")) return false;
            auto* t = new DdcgAst::TypeList(inner);
         t->loc = Loc();
         *result = t;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("tuple")) return false;
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
            if (!match("<")) return false;
            skip();
            if (!parse_TypeRef_(&inner)) return false;
            elems.push_back(inner);
            for (;;) {
                auto _m3 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_TypeRef_(&inner)) return false;
                    elems.push_back(inner);
                    return true;
                }()) { restore(_m3); break; }
            }
            skip();
            if (!match(">")) return false;
            auto* t = new DdcgAst::TypeTuple(std::move(elems));
         t->loc = Loc();
         *result = t;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(name_span)) return false;
        name = name_span.to_string();
        {
            auto _m4 = save();
            if (![&]() -> bool {
                skip();
                if (!match(".")) return false;
                skip();
                if (!ident(sub_span)) return false;
                mod = std::move(name);
                                     name = sub_span.to_string();
                return true;
            }()) restore(_m4);
        }
        {
            auto _m5 = save();
            if (![&]() -> bool {
                skip();
                if (!match("*")) return false;
                is_ptr = true;
                return true;
            }()) restore(_m5);
        }
        auto* t = new DdcgAst::TypeName(std::move(mod), std::move(name), is_ptr);
         t->loc = Loc();
         *result = t;
        }
        }
    }
    return true;
}

bool Parser::parse_RuleDecl(std::vector<DdcgAst::Rule*>& rules) {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::Pattern*> heads;
       DdcgAst::Pattern* p = nullptr;
       DdcgAst::Expr* guard = nullptr;
       std::vector<DdcgAst::Stmt*> body;
    skip();
    if (!match("rule")) return false;
    {
        auto _m0 = save();
        bool _ok0 = [&]() -> bool {
            if (at_end() || !is_ident_continue(peek())) return false;
            advance();
            return true;
        }();
        restore(_m0);
        if (_ok0) return false;
    }
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_Pattern_(&p)) return false;
    heads.push_back(p);
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match(",")) return false;
            skip();
            if (!parse_Pattern_(&p)) return false;
            heads.push_back(p);
            return true;
        }()) { restore(_m1); break; }
    }
    {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!match("where")) return false;
            {
                auto _m3 = save();
                bool _ok3 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m3);
                if (_ok3) return false;
            }
            skip();
            if (!parse_Expr_(&guard)) return false;
            return true;
        }()) restore(_m2);
    }
    skip();
    if (!parse_Block_(body)) return false;
    auto* r = new DdcgAst::Rule(std::move(heads),
             guard ? std::optional<DdcgAst::Expr*>(guard) : std::nullopt,
             std::move(body));
         r->loc = Loc();
         rules.push_back(r);
    return true;
}

bool Parser::parse_Block_(std::vector<DdcgAst::Stmt*>& body) {
    skip();
    if (!match("{")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Stmt_(body)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match("}")) return false;
    return true;
}

bool Parser::parse_Pattern_(DdcgAst::Pattern* * result) {
    peg::Span first_span; peg::Span ctor_span; peg::Span lit_span;
       std::string first; std::string ctor;
       std::vector<DdcgAst::FieldPat*> fields;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!integer(lit_span)) return false;
            int64_t n = std::stoll(lit_span.to_string());
         auto* p = new DdcgAst::IntPat(static_cast<int>(n));
         p->loc = Loc();
         *result = p;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!string_lit(lit_span)) return false;
            std::string s = peg::unescape_string_lit(lit_span);
         auto* p = new DdcgAst::StringPat(std::move(s));
         p->loc = Loc();
         *result = p;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(first_span)) return false;
        first = first_span.to_string();
        {
            auto _m1 = save();
            if ([&]() -> bool {
                skip();
                if (!match(".")) return false;
                skip();
                if (!ident(ctor_span)) return false;
                ctor = ctor_span.to_string();
                skip();
                if (!match("(")) return false;
                {
                    auto _m2 = save();
                    if (![&]() -> bool {
                        skip();
                        if (!parse_FieldPat_(fields)) return false;
                        for (;;) {
                            auto _m3 = save();
                            if (![&]() -> bool {
                                skip();
                                if (!match(",")) return false;
                                skip();
                                if (!parse_FieldPat_(fields)) return false;
                                return true;
                            }()) { restore(_m3); break; }
                        }
                        return true;
                    }()) restore(_m2);
                }
                skip();
                if (!match(")")) return false;
                auto* p = new DdcgAst::ConstructorPat(std::move(first),
               std::move(ctor), std::move(fields));
           p->loc = Loc();
           *result = p;
                return true;
            }()) {} else {
            restore(_m1);
            if ([&]() -> bool {
                skip();
                if (!match("(")) return false;
                {
                    auto _m4 = save();
                    if (![&]() -> bool {
                        skip();
                        if (!parse_FieldPat_(fields)) return false;
                        for (;;) {
                            auto _m5 = save();
                            if (![&]() -> bool {
                                skip();
                                if (!match(",")) return false;
                                skip();
                                if (!parse_FieldPat_(fields)) return false;
                                return true;
                            }()) { restore(_m5); break; }
                        }
                        return true;
                    }()) restore(_m4);
                }
                skip();
                if (!match(")")) return false;
                auto* p = new DdcgAst::ConstructorPat(std::string(),
               std::move(first), std::move(fields));
           p->loc = Loc();
           *result = p;
                return true;
            }()) {} else {
            restore(_m1);
            DdcgAst::Pattern* p;
           if (first == "_")        p = new DdcgAst::WildcardPat();
           else if (first == "nil") p = new DdcgAst::NilPat();
           else                     p = new DdcgAst::BindPat(std::move(first));
           p->loc = Loc();
           *result = p;
            }
            }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_MatchArm_(std::vector<DdcgAst::MatchArm*>& arms) {
    DdcgAst::Pattern* p = nullptr;
       std::vector<DdcgAst::Stmt*> body;
       DdcgAst::Expr* tail = nullptr;
    skip();
    if (!parse_Pattern_(&p)) return false;
    skip();
    if (!match("=>")) return false;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_Block_(body)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_Expr_(&tail)) return false;
        auto* es = new DdcgAst::ExprStmt(tail);
           es->loc = tail->loc;
           body.push_back(es);
        }
    }
    auto* a = new DdcgAst::MatchArm(p, std::move(body));
         a->loc = p->loc;
         arms.push_back(a);
    return true;
}

bool Parser::parse_FieldPat_(std::vector<DdcgAst::FieldPat*>& fields) {
    peg::Span name_span; std::string name;
       DdcgAst::Pattern* p = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("...")) return false;
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            auto* f = new DdcgAst::RestFieldPat(std::move(name));
         f->loc = Loc();
         fields.push_back(f);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(name_span)) return false;
        name = name_span.to_string();
        {
            auto _m1 = save();
            if ([&]() -> bool {
                skip();
                if (!match(":")) return false;
                skip();
                if (!parse_Pattern_(&p)) return false;
                auto* f = new DdcgAst::NamedFieldPat(std::move(name), p);
           f->loc = Loc();
           fields.push_back(f);
                return true;
            }()) {} else {
            restore(_m1);
            auto* bind = new DdcgAst::BindPat(name);
           bind->loc = Loc();
           auto* f = new DdcgAst::NamedFieldPat(std::move(name), bind);
           f->loc = Loc();
           fields.push_back(f);
            }
        }
        }
    }
    return true;
}

bool Parser::parse_Stmt_(std::vector<DdcgAst::Stmt*>& stmts) {
    peg::Span name_span; std::string name;
       std::vector<DdcgAst::Bind*> binds;
       DdcgAst::Bind* b = nullptr;
       DdcgAst::Bind* idx_bind = nullptr;
       DdcgAst::Expr* e = nullptr;
       std::vector<DdcgAst::Stmt*> body;
       std::vector<DdcgAst::Stmt*> else_body;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("let")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            {
                auto _m2 = save();
                if ([&]() -> bool {
                    skip();
                    if (!match("(")) return false;
                    skip();
                    if (!parse_Bind_(&b)) return false;
                    binds.push_back(b);
                    for (;;) {
                        auto _m3 = save();
                        if (![&]() -> bool {
                            skip();
                            if (!match(",")) return false;
                            skip();
                            if (!parse_Bind_(&b)) return false;
                            binds.push_back(b);
                            return true;
                        }()) { restore(_m3); break; }
                    }
                    skip();
                    if (!match(")")) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                skip();
                if (!parse_Bind_(&b)) return false;
                binds.push_back(b);
                }
            }
            skip();
            if (!match("=")) return false;
            skip();
            if (!parse_Expr_(&e)) return false;
            skip();
            if (!match(";")) return false;
            auto* s = new DdcgAst::LetStmt(std::move(binds), e);
         s->loc = Loc();
         stmts.push_back(s);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("for")) return false;
            {
                auto _m4 = save();
                bool _ok4 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m4);
                if (_ok4) return false;
            }
            {
                auto _m5 = save();
                if ([&]() -> bool {
                    skip();
                    if (!match("(")) return false;
                    skip();
                    if (!parse_Bind_(&idx_bind)) return false;
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_Bind_(&b)) return false;
                    skip();
                    if (!match(")")) return false;
                    return true;
                }()) {} else {
                restore(_m5);
                skip();
                if (!parse_Bind_(&b)) return false;
                }
            }
            skip();
            if (!match("in")) return false;
            {
                auto _m6 = save();
                bool _ok6 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m6);
                if (_ok6) return false;
            }
            skip();
            if (!parse_Expr_(&e)) return false;
            skip();
            if (!parse_Block_(body)) return false;
            auto* s = new DdcgAst::ForStmt(b,
             idx_bind ? std::optional<DdcgAst::Bind*>(idx_bind) : std::nullopt,
             e, std::move(body));
         s->loc = Loc();
         stmts.push_back(s);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("if")) return false;
            {
                auto _m7 = save();
                bool _ok7 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m7);
                if (_ok7) return false;
            }
            skip();
            if (!parse_Expr_(&e)) return false;
            skip();
            if (!parse_Block_(body)) return false;
            {
                auto _m8 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("else")) return false;
                    {
                        auto _m9 = save();
                        bool _ok9 = [&]() -> bool {
                            if (at_end() || !is_ident_continue(peek())) return false;
                            advance();
                            return true;
                        }();
                        restore(_m9);
                        if (_ok9) return false;
                    }
                    skip();
                    if (!parse_Block_(else_body)) return false;
                    return true;
                }()) restore(_m8);
            }
            auto* s = new DdcgAst::IfStmt(e, std::move(body), std::move(else_body));
         s->loc = Loc();
         stmts.push_back(s);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("label")) return false;
            {
                auto _m10 = save();
                bool _ok10 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m10);
                if (_ok10) return false;
            }
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            skip();
            if (!match(";")) return false;
            auto* s = new DdcgAst::LabelStmt(std::move(name));
         s->loc = Loc();
         stmts.push_back(s);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            skip();
            if (!match(":=")) return false;
            skip();
            if (!parse_Expr_(&e)) return false;
            skip();
            if (!match(";")) return false;
            auto* s = new DdcgAst::MutAssign(std::move(name), e);
         s->loc = Loc();
         stmts.push_back(s);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_Expr_(&e)) return false;
        skip();
        if (!match(";")) return false;
        auto* s = new DdcgAst::ExprStmt(e);
         s->loc = Loc();
         stmts.push_back(s);
        }
        }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_Bind_(DdcgAst::Bind* * result) {
    peg::Span name_span; std::string name;
       DdcgAst::TypeRef* ty = nullptr;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match(":")) return false;
            skip();
            if (!parse_TypeRef_(&ty)) return false;
            return true;
        }()) restore(_m0);
    }
    DdcgAst::Bind* b;
         if (name == "_") {
             b = new DdcgAst::WildcardBind();
         } else {
             b = new DdcgAst::SimpleBind(std::move(name),
                 ty ? std::optional<DdcgAst::TypeRef*>(ty) : std::nullopt);
         }
         b->loc = Loc();
         *result = b;
    return true;
}

bool Parser::parse_Expr_(DdcgAst::Expr* * result) {
    skip();
    if (!parse_TernaryExpr(result)) return false;
    return true;
}

bool Parser::parse_TernaryExpr(DdcgAst::Expr* * result) {
    DdcgAst::Expr* then_ = nullptr;
       DdcgAst::Expr* else_ = nullptr;
    skip();
    if (!parse_LogicalOr(result)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("?")) return false;
            skip();
            if (!parse_Expr_(&then_)) return false;
            skip();
            if (!match(":")) return false;
            skip();
            if (!parse_TernaryExpr(&else_)) return false;
            auto* t = new DdcgAst::TernaryExpr(*result, then_, else_);
           t->loc = (*result)->loc;
           *result = t;
            return true;
        }()) restore(_m0);
    }
    return true;
}

bool Parser::parse_LogicalOr(DdcgAst::Expr* * result) {
    DdcgAst::Expr* rhs = nullptr;
    skip();
    if (!parse_LogicalAnd(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("or")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!parse_LogicalAnd(&rhs)) return false;
            auto* b = new DdcgAst::BinOp("or", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_LogicalAnd(DdcgAst::Expr* * result) {
    DdcgAst::Expr* rhs = nullptr;
    skip();
    if (!parse_Equality(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("and")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!parse_Equality(&rhs)) return false;
            auto* b = new DdcgAst::BinOp("and", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Equality(DdcgAst::Expr* * result) {
    DdcgAst::Expr* rhs = nullptr;
    skip();
    if (!parse_Relational(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("==")) {
                skip();
                if (!match("==")) return false;
                skip();
                if (!parse_Relational(&rhs)) return false;
                auto* b = new DdcgAst::BinOp("==", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            } else {
                skip();
                if (!match("!=")) return false;
                skip();
                if (!parse_Relational(&rhs)) return false;
                auto* b = new DdcgAst::BinOp("!=", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Relational(DdcgAst::Expr* * result) {
    DdcgAst::Expr* rhs = nullptr;
    skip();
    if (!parse_Additive(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!match("<=")) return false;
                    skip();
                    if (!parse_Additive(&rhs)) return false;
                    auto* b = new DdcgAst::BinOp("<=", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match(">=")) return false;
                    skip();
                    if (!parse_Additive(&rhs)) return false;
                    auto* b = new DdcgAst::BinOp(">=", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match("<")) return false;
                    skip();
                    if (!parse_Additive(&rhs)) return false;
                    auto* b = new DdcgAst::BinOp("<", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!match(">")) return false;
                skip();
                if (!parse_Additive(&rhs)) return false;
                auto* b = new DdcgAst::BinOp(">", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
                }
                }
                }
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Additive(DdcgAst::Expr* * result) {
    DdcgAst::Expr* rhs = nullptr;
    skip();
    if (!parse_Multiplicative(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("+")) {
                skip();
                if (!match("+")) return false;
                skip();
                if (!parse_Multiplicative(&rhs)) return false;
                auto* b = new DdcgAst::BinOp("+", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            } else {
                skip();
                if (!match("-")) return false;
                skip();
                if (!parse_Multiplicative(&rhs)) return false;
                auto* b = new DdcgAst::BinOp("-", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Multiplicative(DdcgAst::Expr* * result) {
    DdcgAst::Expr* rhs = nullptr;
    skip();
    if (!parse_Unary(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("*")) {
                skip();
                if (!match("*")) return false;
                skip();
                if (!parse_Unary(&rhs)) return false;
                auto* b = new DdcgAst::BinOp("*", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            } else             if (peek_at("/")) {
                skip();
                if (!match("/")) return false;
                skip();
                if (!parse_Unary(&rhs)) return false;
                auto* b = new DdcgAst::BinOp("/", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            } else {
                skip();
                if (!match("%")) return false;
                skip();
                if (!parse_Unary(&rhs)) return false;
                auto* b = new DdcgAst::BinOp("%", *result, rhs);
           b->loc = rhs->loc;
           *result = b;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Unary(DdcgAst::Expr* * result) {
    DdcgAst::Expr* operand = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("not")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!parse_Unary(&operand)) return false;
            auto* u = new DdcgAst::UnaryOp("not", operand);
         u->loc = operand->loc;
         *result = u;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("-")) return false;
            skip();
            if (!parse_Unary(&operand)) return false;
            auto* u = new DdcgAst::UnaryOp("-", operand);
         u->loc = operand->loc;
         *result = u;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_Postfix(result)) return false;
        }
        }
    }
    return true;
}

bool Parser::parse_Postfix(DdcgAst::Expr* * result) {
    DdcgAst::Expr* idx = nullptr;
       std::vector<DdcgAst::Expr*> args;
       DdcgAst::Expr* arg = nullptr;
       DdcgAst::Expr* with_val = nullptr;
       peg::Span field_span; std::string field;
    skip();
    if (!parse_Primary(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!match("(")) return false;
                    {
                        auto _m2 = save();
                        if (![&]() -> bool {
                            skip();
                            if (!parse_Expr_(&arg)) return false;
                            args.push_back(arg);
                            for (;;) {
                                auto _m3 = save();
                                if (![&]() -> bool {
                                    skip();
                                    if (!match(",")) return false;
                                    skip();
                                    if (!parse_Expr_(&arg)) return false;
                                    args.push_back(arg);
                                    return true;
                                }()) { restore(_m3); break; }
                            }
                            return true;
                        }()) restore(_m2);
                    }
                    skip();
                    if (!match(")")) return false;
                    auto* c = new DdcgAst::CallExpr(*result, std::move(args));
           c->loc = (*result)->loc;
           *result = c;
           args.clear();
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match(".")) return false;
                    skip();
                    if (!ident(field_span)) return false;
                    field = field_span.to_string();
                    auto* f = new DdcgAst::FieldAccExpr(*result, std::move(field));
           f->loc = (*result)->loc;
           *result = f;
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match("[")) return false;
                    skip();
                    if (!parse_Expr_(&idx)) return false;
                    skip();
                    if (!match("]")) return false;
                    auto* x = new DdcgAst::IndexAccExpr(*result, idx);
           x->loc = (*result)->loc;
           *result = x;
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!match("with")) return false;
                {
                    auto _m4 = save();
                    bool _ok4 = [&]() -> bool {
                        if (at_end() || !is_ident_continue(peek())) return false;
                        advance();
                        return true;
                    }();
                    restore(_m4);
                    if (_ok4) return false;
                }
                skip();
                if (!ident(field_span)) return false;
                field = field_span.to_string();
                skip();
                if (!match(":=")) return false;
                skip();
                if (!parse_Expr_(&with_val)) return false;
                auto* w = new DdcgAst::WithExpr(*result, std::move(field), with_val);
           w->loc = (*result)->loc;
           *result = w;
                }
                }
                }
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Primary(DdcgAst::Expr* * result) {
    peg::Span name_span; peg::Span lit_span;
       std::string name;
       DdcgAst::Expr* inner = nullptr;
       std::vector<DdcgAst::Expr*> elems;
       DdcgAst::Expr* elem = nullptr;
       DdcgAst::Expr* subj = nullptr;
       DdcgAst::Expr* arg = nullptr;
       std::vector<DdcgAst::Expr*> gen_args;
       std::vector<DdcgAst::Stmt*> block_body;
       std::vector<DdcgAst::MatchArm*> arms;
       peg::Span schema_span; peg::Span ctor_span;
       std::string schema_name; std::string ctor_name;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("true")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            auto* b = new DdcgAst::BoolLit(true);
         b->loc = Loc();
         *result = b;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("false")) return false;
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
            auto* b = new DdcgAst::BoolLit(false);
         b->loc = Loc();
         *result = b;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("nil")) return false;
            {
                auto _m3 = save();
                bool _ok3 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m3);
                if (_ok3) return false;
            }
            auto* n = new DdcgAst::NilLit();
         n->loc = Loc();
         *result = n;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("gen")) return false;
            {
                auto _m4 = save();
                bool _ok4 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m4);
                if (_ok4) return false;
            }
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr_(&subj)) return false;
            for (;;) {
                auto _m5 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_Expr_(&arg)) return false;
                    gen_args.push_back(arg);
                    return true;
                }()) { restore(_m5); break; }
            }
            skip();
            if (!match(")")) return false;
            auto* g = new DdcgAst::GenCall(subj, std::move(gen_args));
         g->loc = subj->loc;
         *result = g;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("match")) return false;
            {
                auto _m6 = save();
                bool _ok6 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m6);
                if (_ok6) return false;
            }
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr_(&subj)) return false;
            skip();
            if (!match(")")) return false;
            skip();
            if (!match("{")) return false;
            skip();
            if (!parse_MatchArm_(arms)) return false;
            for (;;) {
                auto _m7 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("|")) return false;
                    skip();
                    if (!parse_MatchArm_(arms)) return false;
                    return true;
                }()) { restore(_m7); break; }
            }
            skip();
            if (!match("}")) return false;
            auto* m = new DdcgAst::MatchExpr(subj, std::move(arms));
         m->loc = subj->loc;
         *result = m;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("build")) return false;
            {
                auto _m8 = save();
                bool _ok8 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m8);
                if (_ok8) return false;
            }
            skip();
            if (!ident(schema_span)) return false;
            schema_name = schema_span.to_string();
            skip();
            if (!match(".")) return false;
            skip();
            if (!ident(ctor_span)) return false;
            ctor_name = ctor_span.to_string();
            skip();
            if (!match("(")) return false;
            {
                auto _m9 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_Expr_(&elem)) return false;
                    elems.push_back(elem);
                    for (;;) {
                        auto _m10 = save();
                        if (![&]() -> bool {
                            skip();
                            if (!match(",")) return false;
                            skip();
                            if (!parse_Expr_(&elem)) return false;
                            elems.push_back(elem);
                            return true;
                        }()) { restore(_m10); break; }
                    }
                    return true;
                }()) restore(_m9);
            }
            skip();
            if (!match(")")) return false;
            auto* b = new DdcgAst::BuildExpr(std::move(schema_name),
             std::move(ctor_name), std::move(elems));
         b->loc = Loc();
         *result = b;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!integer(lit_span)) return false;
            int64_t n = std::stoll(lit_span.to_string());
         auto* i = new DdcgAst::IntLit(static_cast<int>(n));
         i->loc = Loc();
         *result = i;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!string_lit(lit_span)) return false;
            std::string s = peg::unescape_string_lit(lit_span);
         auto* sl = new DdcgAst::StringLit(std::move(s));
         sl->loc = Loc();
         *result = sl;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("[")) return false;
            {
                auto _m11 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_Expr_(&elem)) return false;
                    elems.push_back(elem);
                    for (;;) {
                        auto _m12 = save();
                        if (![&]() -> bool {
                            skip();
                            if (!match(",")) return false;
                            skip();
                            if (!parse_Expr_(&elem)) return false;
                            elems.push_back(elem);
                            return true;
                        }()) { restore(_m12); break; }
                    }
                    return true;
                }()) restore(_m11);
            }
            skip();
            if (!match("]")) return false;
            auto* l = new DdcgAst::ListLit(std::move(elems));
         l->loc = Loc();
         *result = l;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_Block_(block_body)) return false;
            auto* be = new DdcgAst::BlockExpr(std::move(block_body));
         be->loc = Loc();
         *result = be;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr_(&inner)) return false;
            skip();
            if (peek_at(")")) {
                skip();
                if (!match(")")) return false;
                *result = inner;
            } else {
                skip();
                if (!match(",")) return false;
                skip();
                if (!parse_Expr_(&elem)) return false;
                elems.push_back(inner); elems.push_back(elem);
                for (;;) {
                    auto _m13 = save();
                    if (![&]() -> bool {
                        skip();
                        if (!match(",")) return false;
                        skip();
                        if (!parse_Expr_(&elem)) return false;
                        elems.push_back(elem);
                        return true;
                    }()) { restore(_m13); break; }
                }
                skip();
                if (!match(")")) return false;
                auto* t = new DdcgAst::TupleLit(std::move(elems));
           t->loc = inner->loc;
           *result = t;
            }
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(name_span)) return false;
        name = name_span.to_string();
        auto* id = new DdcgAst::IdentExpr(std::move(name));
         id->loc = Loc();
         *result = id;
        }
        }
        }
        }
        }
        }
        }
        }
        }
        }
        }
    }
    return true;
}


