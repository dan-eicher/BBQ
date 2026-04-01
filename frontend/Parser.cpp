#include "Parser.h"


using namespace BBQ;


void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_BBQ();
}

void Parser::setup_skip() {
    set_whitespace([](char c) -> bool {
        return ((((c == 13) || (c == 10)) || (c == 9)) || (c == 32));
    });
    set_comments({{"#", "\n", false}, {"//", "\n", false}});
}


bool Parser::parse_BBQ() {
    std::vector<EndianDirective*> dirs;
       std::vector<Rule*> rules;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Directive(&dirs)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!parse_Rule_(&rules)) return false;
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Rule_(&rules)) return false;
            return true;
        }()) { restore(_m1); break; }
    }
    skip(); if (!at_end()) return false;
    ast = new Grammar(std::move(dirs), std::move(rules));
       ast->loc = SourceLoc{nullptr, 1, 1};
    return true;
}

bool Parser::parse_Directive(std::vector<EndianDirective*> * dirs) {
    Endianness e;
    skip();
    if (!match("@endian")) return false;
    skip();
    if (peek_keyword("big")) {
        skip();
        if (!keyword("big")) return false;
        e = Endianness::Big;
    } else {
        skip();
        if (!keyword("little")) return false;
        e = Endianness::Little;
    }
    auto* d = new EndianDirective(e);
       d->loc = Loc();
       dirs->push_back(d);
    return true;
}

bool Parser::parse_Rule_(std::vector<Rule*> * rules) {
    std::string name; TypeExpr* body;
    skip();
    if (!ident(name)) return false;
    SourceLoc loc = Loc();
    skip();
    if (!match("=")) return false;
    skip();
    if (!parse_TypeBody(&body)) return false;
    auto* r = new Rule(std::move(name), body);
       r->loc = loc;
       rules->push_back(r);
    return true;
}

bool Parser::parse_TypeBody(TypeExpr* * result) {
    TypeExpr* first;
       std::vector<TypeExpr*> alts;
    skip();
    if (!parse_TypeTerm(&first)) return false;
    alts.push_back(first);
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("|")) return false;
            skip();
            if (!parse_TypeTerm(&first)) return false;
            alts.push_back(first);
            return true;
        }()) { restore(_m0); break; }
    }
    if (alts.size() == 1)
           *result = alts[0];
       else {
           auto loc = alts[0]->loc;
           *result = new Alternatives(std::move(alts));
           (*result)->loc = loc;
       }
    return true;
}

bool Parser::parse_TypeTerm(TypeExpr* * result) {
    Expr* constr = nullptr;
       Interval* iv = nullptr;
    skip();
    if (!parse_TypePrimary(result)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    return true;
}

bool Parser::parse_TypePrimary(TypeExpr* * result) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!keyword("struct")) return false;
            skip();
            if (!parse_StructBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("union")) return false;
            skip();
            if (!parse_UnionBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("array")) return false;
            skip();
            if (!parse_ArrayBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("optional")) return false;
            skip();
            if (!parse_OptionalBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("switch")) return false;
            skip();
            if (!parse_SwitchBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("compute")) return false;
            skip();
            if (!parse_ComputeBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("extern")) return false;
            skip();
            if (!parse_ExternBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("bitfield")) return false;
            skip();
            if (!parse_BitfieldBody(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_PrimitiveType(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_TypeBody(result)) return false;
            skip();
            if (!match(")")) return false;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_RuleRefType(result)) return false;
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

bool Parser::parse_StructBody(TypeExpr* * result) {
    std::vector<Field*> fields;
       SourceLoc loc = Loc();
    skip();
    if (!match("{")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_StructElement(&fields)) return false;
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_StructElement(&fields)) return false;
                    return true;
                }()) { restore(_m1); break; }
            }
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    return true;
                }()) restore(_m2);
            }
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match("}")) return false;
    *result = new Struct(std::move(fields));
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_StructElement(std::vector<Field*> * fields) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_EndianDirectiveField(fields)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_FieldDecl(fields)) return false;
        }
    }
    return true;
}

bool Parser::parse_EndianDirectiveField(std::vector<Field*> * fields) {
    Expr* expr;
    skip();
    if (!match("@endian")) return false;
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_Expr_(&expr)) return false;
    auto* es = new EndianSwitch(expr);
       es->loc = Loc();
       auto* f = new Field("", es, std::nullopt, std::nullopt);
       f->loc = es->loc;
       fields->push_back(f);
    return true;
}

bool Parser::parse_FieldDecl(std::vector<Field*> * fields) {
    std::string name;
       TypeExpr* body;
       Expr* constr = nullptr;
       Interval* iv = nullptr;
    skip();
    if (!ident(name)) return false;
    SourceLoc loc = Loc();
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TypeBody(&body)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    auto* f = new Field(std::move(name), body,
           constr ? std::optional<Expr*>(constr) : std::nullopt,
           iv     ? std::optional<Interval*>(iv) : std::nullopt);
       f->loc = loc;
       fields->push_back(f);
    return true;
}

bool Parser::parse_UnionBody(TypeExpr* * result) {
    std::vector<Variant*> variants;
       SourceLoc loc = Loc();
    skip();
    if (!match("{")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_VariantDecl(&variants)) return false;
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_VariantDecl(&variants)) return false;
                    return true;
                }()) { restore(_m1); break; }
            }
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    return true;
                }()) restore(_m2);
            }
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match("}")) return false;
    *result = new Union(std::move(variants));
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_VariantDecl(std::vector<Variant*> * variants) {
    std::string name;
       TypeExpr* body;
       Expr* constr = nullptr;
       Interval* iv = nullptr;
    skip();
    if (!ident(name)) return false;
    SourceLoc loc = Loc();
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TypeBody(&body)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    auto* v = new Variant(std::move(name), body,
           constr ? std::optional<Expr*>(constr) : std::nullopt,
           iv     ? std::optional<Interval*>(iv) : std::nullopt);
       v->loc = loc;
       variants->push_back(v);
    return true;
}

bool Parser::parse_ArrayBody(TypeExpr* * result) {
    TypeExpr* elem;
       ArraySpec* spec;
       Expr* constr = nullptr;
       Interval* iv = nullptr;
       Interval* elem_iv = nullptr;
       SourceLoc loc = Loc();
    skip();
    if (!match("<")) return false;
    skip();
    if (!parse_TypeBody(&elem)) return false;
    skip();
    if (!match(">")) return false;
    skip();
    if (!parse_ArraySpecRule(&spec)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!match("@")) return false;
            skip();
            if (!parse_ElementIntervalSpec(&elem_iv)) return false;
            return true;
        }()) restore(_m2);
    }
    *result = new Array(elem, spec,
           constr ? std::optional<Expr*>(constr) : std::nullopt,
           iv     ? std::optional<Interval*>(iv) : std::nullopt,
           elem_iv ? std::optional<Interval*>(elem_iv) : std::nullopt);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_ArraySpecRule(ArraySpec* * result) {
    Expr* count;
       Separator* sep;
       Terminator* term;
    skip();
    if (peek_at("[")) {
        skip();
        if (!match("[")) return false;
        skip();
        if (!parse_Expr_(&count)) return false;
        skip();
        if (!match("]")) return false;
        *result = new FixedCount(count); (*result)->loc = count->loc;
    } else {
        skip();
        if (!match("(")) return false;
        skip();
        if (!parse_SepSpec(&sep)) return false;
        skip();
        if (!match(",")) return false;
        skip();
        if (!parse_TermSpec(&term)) return false;
        skip();
        if (!match(")")) return false;
        *result = new SepTerm(sep, term); (*result)->loc = sep->loc;
    }
    return true;
}

bool Parser::parse_SepSpec(Separator* * result) {
    TypeExpr* ty;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!keyword("none")) return false;
            *result = new NoneSep(); (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_TypeBody(&ty)) return false;
        auto* s = new TypeSep(ty); s->loc = ty->loc; *result = s;
        }
    }
    return true;
}

bool Parser::parse_TermSpec(Terminator* * result) {
    Expr* e; TypeExpr* ty;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!keyword("eof")) return false;
            *result = new EofTerm(); (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("count")) return false;
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr_(&e)) return false;
            skip();
            if (!match(")")) return false;
            *result = new CountTerm(e); (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("until")) return false;
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr_(&e)) return false;
            skip();
            if (!match(")")) return false;
            *result = new UntilTerm(e); (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_TypeBody(&ty)) return false;
        auto* tt = new BBQ::TypeTerm(ty); tt->loc = ty->loc; *result = tt;
        }
        }
        }
    }
    return true;
}

bool Parser::parse_OptionalBody(TypeExpr* * result) {
    TypeExpr* elem;
       Expr* constr = nullptr;
       Interval* iv = nullptr;
       SourceLoc loc = Loc();
    skip();
    if (!match("<")) return false;
    skip();
    if (!parse_TypeBody(&elem)) return false;
    skip();
    if (!match(">")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    *result = new Optional(elem,
           constr ? std::optional<Expr*>(constr) : std::nullopt,
           iv     ? std::optional<Interval*>(iv) : std::nullopt);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_SwitchBody(TypeExpr* * result) {
    Expr* disc;
       std::vector<SwitchCase*> cases;
       SwitchDefault* def = nullptr;
       SourceLoc loc = Loc();
    skip();
    if (!match("(")) return false;
    skip();
    if (!parse_Expr_(&disc)) return false;
    skip();
    if (!match(")")) return false;
    skip();
    if (!match("{")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_SwitchCaseRule(&cases)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_DefaultCaseRule(&def)) return false;
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!match("}")) return false;
    *result = new Switch(disc, std::move(cases),
           def ? std::optional<SwitchDefault*>(def) : std::nullopt);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_SwitchCaseRule(std::vector<SwitchCase*> * cases) {
    CaseValue* cv;
       TypeExpr* target;
       Interval* iv = nullptr;
    skip();
    if (!parse_CaseValueRule(&cv)) return false;
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TypeBody(&target)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match(";")) return false;
    auto* sc = new SwitchCase(cv, target,
           iv ? std::optional<Interval*>(iv) : std::nullopt);
       sc->loc = cv->loc;
       cases->push_back(sc);
    return true;
}

bool Parser::parse_DefaultCaseRule(SwitchDefault* * result) {
    TypeExpr* target;
       Interval* iv = nullptr;
    skip();
    if (!keyword("default")) return false;
    SourceLoc loc = Loc();
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TypeBody(&target)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match(";")) return false;
    *result = new SwitchDefault(target,
           iv ? std::optional<Interval*>(iv) : std::nullopt);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_CaseValueRule(CaseValue* * result) {
    std::vector<std::string> path;
       std::string s;
       int64_t ival;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_DottedRef(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!integer(ival)) return false;
            *result = new IntValue(ival); (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!string_lit(s)) return false;
            *result = new StrValue(std::move(s)); (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        {
            auto _m1 = save();
            bool _ok1 = [&]() -> bool {
                skip();
                if (!keyword("default")) return false;
                return true;
            }();
            restore(_m1);
            if (_ok1) return false;
        }
        skip();
        if (!ident(s)) return false;
        *result = new IdentValue(std::move(s)); (*result)->loc = Loc();
        }
        }
        }
    }
    return true;
}

bool Parser::parse_DottedRef(CaseValue* * result) {
    std::vector<std::string> path;
       std::string s;
    skip();
    if (!ident(s)) return false;
    path.push_back(std::move(s));
    skip();
    if (!match(".")) return false;
    skip();
    if (!ident(s)) return false;
    path.push_back(std::move(s));
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match(".")) return false;
            skip();
            if (!ident(s)) return false;
            path.push_back(std::move(s));
            return true;
        }()) { restore(_m0); break; }
    }
    *result = new RefValue(std::move(path)); (*result)->loc = Loc();
    return true;
}

bool Parser::parse_ComputeBody(TypeExpr* * result) {
    Expr* e;
       PrimitiveKind* pk;
       SourceLoc loc = Loc();
    skip();
    if (!match("(")) return false;
    skip();
    if (!parse_Expr_(&e)) return false;
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_PrimKeyword(&pk)) return false;
    skip();
    if (!match(")")) return false;
    *result = new Compute(e, pk);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_ExternBody(TypeExpr* * result) {
    std::string func; std::string ctype;
       Expr* constr = nullptr; Interval* iv = nullptr;
       SourceLoc loc = Loc();
    skip();
    if (!match("(")) return false;
    skip();
    if (!string_lit(func)) return false;
    skip();
    if (!match(",")) return false;
    skip();
    if (!string_lit(ctype)) return false;
    skip();
    if (!match(")")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    *result = new Extern(std::move(func), std::move(ctype),
           constr ? std::optional<Expr*>(constr) : std::nullopt,
           iv     ? std::optional<Interval*>(iv) : std::nullopt);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_BitfieldBody(TypeExpr* * result) {
    PrimitiveKind* container;
       std::vector<BitfieldEntry*> entries;
       SourceLoc loc = Loc();
    skip();
    if (!match("<")) return false;
    skip();
    if (!parse_PrimKeyword(&container)) return false;
    skip();
    if (!match(">")) return false;
    skip();
    if (!match("{")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_BitfieldEntryRule(&entries)) return false;
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_BitfieldEntryRule(&entries)) return false;
                    return true;
                }()) { restore(_m1); break; }
            }
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    return true;
                }()) restore(_m2);
            }
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match("}")) return false;
    *result = new Bitfield(container, std::move(entries));
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_BitfieldEntryRule(std::vector<BitfieldEntry*> * entries) {
    std::string name; int64_t width;
    skip();
    if (!ident(name)) return false;
    SourceLoc loc = Loc();
    skip();
    if (!match(":")) return false;
    skip();
    if (!integer(width)) return false;
    auto* e = new BitfieldEntry(std::move(name), width);
       e->loc = loc;
       entries->push_back(e);
    return true;
}

bool Parser::parse_PrimitiveType(TypeExpr* * result) {
    PrimitiveKind* pk;
       Expr* constr = nullptr;
       Interval* iv = nullptr;
    skip();
    if (!parse_PrimKeyword(&pk)) return false;
    SourceLoc loc = pk->loc;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    *result = new Primitive(pk,
           constr ? std::optional<Expr*>(constr) : std::nullopt,
           iv     ? std::optional<Interval*>(iv) : std::nullopt);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_PrimKeyword(PrimitiveKind* * result) {
    std::string s;
    skip();
    if (!ident(s)) return false;
    *result = DecodePrimitive(s, Loc());
         if (!*result) return false;
    return true;
}

bool Parser::parse_RuleRefType(TypeExpr* * result) {
    std::string name;
       Expr* constr = nullptr;
       Interval* iv = nullptr;
    skip();
    if (!ident(name)) return false;
    SourceLoc loc = Loc();
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ConstraintBlock(&constr)) return false;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_IntervalSpec(&iv)) return false;
            return true;
        }()) restore(_m1);
    }
    *result = new RuleRef(std::move(name),
           constr ? std::optional<Expr*>(constr) : std::nullopt,
           iv     ? std::optional<Interval*>(iv) : std::nullopt);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_ConstraintBlock(Expr* * result) {
    skip();
    if (!keyword("where")) return false;
    skip();
    if (!parse_Expr_(result)) return false;
    return true;
}

bool Parser::parse_IntervalSpec(Interval* * result) {
    Expr* a; Expr* b;
    skip();
    if (!match("[")) return false;
    skip();
    if (!parse_Expr_(&a)) return false;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match(",")) return false;
            skip();
            if (!parse_Expr_(&b)) return false;
            *result = new StartEnd(a, b); (*result)->loc = a->loc;
            return true;
        }()) {} else {
        restore(_m0);
        *result = new Length(a); (*result)->loc = a->loc;
        }
    }
    skip();
    if (!match("]")) return false;
    return true;
}

bool Parser::parse_ElementIntervalSpec(Interval* * result) {
    Expr* a; Expr* b;
    skip();
    if (!match("[")) return false;
    skip();
    if (!parse_Expr_(&a)) return false;
    skip();
    if (!match(",")) return false;
    skip();
    if (!parse_Expr_(&b)) return false;
    skip();
    if (!match("]")) return false;
    *result = new StartEnd(a, b); (*result)->loc = a->loc;
    return true;
}

bool Parser::parse_Expr_(Expr* * result) {
    skip();
    if (!parse_TernaryExpr(result)) return false;
    return true;
}

bool Parser::parse_TernaryExpr(Expr* * result) {
    Expr* then_; Expr* else_;
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
            *result = new Ternary(*result, then_, else_);
           (*result)->loc = then_->loc;
            return true;
        }()) restore(_m0);
    }
    return true;
}

bool Parser::parse_LogicalOr(Expr* * result) {
    Expr* rhs;
    skip();
    if (!parse_LogicalAnd(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("||")) return false;
            skip();
            if (!parse_LogicalAnd(&rhs)) return false;
            *result = new BinOp(*result, Binop::Or, rhs);
           (*result)->loc = rhs->loc;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_LogicalAnd(Expr* * result) {
    Expr* rhs;
    skip();
    if (!parse_BitwiseOr(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("&&")) return false;
            skip();
            if (!parse_BitwiseOr(&rhs)) return false;
            *result = new BinOp(*result, Binop::And, rhs);
           (*result)->loc = rhs->loc;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_BitwiseOr(Expr* * result) {
    Expr* rhs;
    skip();
    if (!parse_BitwiseXor(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("|")) return false;
            skip();
            if (!parse_BitwiseXor(&rhs)) return false;
            *result = new BinOp(*result, Binop::BitOr, rhs);
           (*result)->loc = rhs->loc;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_BitwiseXor(Expr* * result) {
    Expr* rhs;
    skip();
    if (!parse_BitwiseAnd(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("^")) return false;
            skip();
            if (!parse_BitwiseAnd(&rhs)) return false;
            *result = new BinOp(*result, Binop::BitXor, rhs);
           (*result)->loc = rhs->loc;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_BitwiseAnd(Expr* * result) {
    Expr* rhs;
    skip();
    if (!parse_Equality(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("&")) return false;
            skip();
            if (!parse_Equality(&rhs)) return false;
            *result = new BinOp(*result, Binop::BitAnd, rhs);
           (*result)->loc = rhs->loc;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Equality(Expr* * result) {
    Expr* rhs;
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
                *result = new BinOp(*result, Binop::Eq, rhs);
           (*result)->loc = rhs->loc;
            } else {
                skip();
                if (!match("!=")) return false;
                skip();
                if (!parse_Relational(&rhs)) return false;
                *result = new BinOp(*result, Binop::Ne, rhs);
           (*result)->loc = rhs->loc;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Relational(Expr* * result) {
    Expr* rhs;
    skip();
    if (!parse_Shift(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("<=")) {
                skip();
                if (!match("<=")) return false;
                skip();
                if (!parse_Shift(&rhs)) return false;
                *result = new BinOp(*result, Binop::Le, rhs);
           (*result)->loc = rhs->loc;
            } else             if (peek_at(">=")) {
                skip();
                if (!match(">=")) return false;
                skip();
                if (!parse_Shift(&rhs)) return false;
                *result = new BinOp(*result, Binop::Ge, rhs);
           (*result)->loc = rhs->loc;
            } else             if (peek_at("<")) {
                skip();
                if (!match("<")) return false;
                {
                    auto _m1 = save();
                    bool _ok1 = [&]() -> bool {
                        skip();
                        if (!match("<")) return false;
                        return true;
                    }();
                    restore(_m1);
                    if (_ok1) return false;
                }
                skip();
                if (!parse_Shift(&rhs)) return false;
                *result = new BinOp(*result, Binop::Lt, rhs);
           (*result)->loc = rhs->loc;
            } else {
                skip();
                if (!match(">")) return false;
                {
                    auto _m2 = save();
                    bool _ok2 = [&]() -> bool {
                        skip();
                        if (!match(">")) return false;
                        return true;
                    }();
                    restore(_m2);
                    if (_ok2) return false;
                }
                skip();
                if (!parse_Shift(&rhs)) return false;
                *result = new BinOp(*result, Binop::Gt, rhs);
           (*result)->loc = rhs->loc;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Shift(Expr* * result) {
    Expr* rhs;
    skip();
    if (!parse_Additive(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("<<")) {
                skip();
                if (!match("<<")) return false;
                skip();
                if (!parse_Additive(&rhs)) return false;
                *result = new BinOp(*result, Binop::Shl, rhs);
           (*result)->loc = rhs->loc;
            } else {
                skip();
                if (!match(">>")) return false;
                skip();
                if (!parse_Additive(&rhs)) return false;
                *result = new BinOp(*result, Binop::Shr, rhs);
           (*result)->loc = rhs->loc;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Additive(Expr* * result) {
    Expr* rhs;
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
                *result = new BinOp(*result, Binop::Add, rhs);
           (*result)->loc = rhs->loc;
            } else {
                skip();
                if (!match("-")) return false;
                skip();
                if (!parse_Multiplicative(&rhs)) return false;
                *result = new BinOp(*result, Binop::Sub, rhs);
           (*result)->loc = rhs->loc;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Multiplicative(Expr* * result) {
    Expr* rhs;
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
                *result = new BinOp(*result, Binop::Mul, rhs);
           (*result)->loc = rhs->loc;
            } else             if (peek_at("/")) {
                skip();
                if (!match("/")) return false;
                skip();
                if (!parse_Unary(&rhs)) return false;
                *result = new BinOp(*result, Binop::Div, rhs);
           (*result)->loc = rhs->loc;
            } else {
                skip();
                if (!match("%")) return false;
                skip();
                if (!parse_Unary(&rhs)) return false;
                *result = new BinOp(*result, Binop::Mod, rhs);
           (*result)->loc = rhs->loc;
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_Unary(Expr* * result) {
    Expr* operand; Unaryop op;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (peek_at("-")) {
                skip();
                if (!match("-")) return false;
                op = Unaryop::Neg;
            } else             if (peek_at("~")) {
                skip();
                if (!match("~")) return false;
                op = Unaryop::BitNot;
            } else             if (peek_at("!")) {
                skip();
                if (!match("!")) return false;
                op = Unaryop::Not;
            } else {
                skip();
                if (!keyword("abs")) return false;
                op = Unaryop::Abs;
            }
            skip();
            if (!parse_Postfix(&operand)) return false;
            *result = new UnaryOp(op, operand);
         (*result)->loc = operand->loc;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_Postfix(result)) return false;
        }
    }
    return true;
}

bool Parser::parse_Postfix(Expr* * result) {
    Expr* idx; std::string fname;
    skip();
    if (!parse_PrimaryExpr(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("[")) {
                skip();
                if (!match("[")) return false;
                skip();
                if (!parse_Expr_(&idx)) return false;
                skip();
                if (!match("]")) return false;
                auto* ref = dynamic_cast<Ref*>(*result);
           if (ref) {
               auto* ia = new IndexAcc(ref->path, idx);
               ia->loc = ref->loc;
               ref->path = ia;
           }
            } else {
                skip();
                if (!match(".")) return false;
                skip();
                if (!ident(fname)) return false;
                auto* ref = dynamic_cast<Ref*>(*result);
           if (ref) {
               auto* fa = new FieldAcc(ref->path, std::move(fname));
               fa->loc = ref->loc;
               ref->path = fa;
           }
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_PrimaryExpr(Expr* * result) {
    std::string name;
       std::vector<Expr*> args;
       Expr* arg;
       int64_t ival;
       double fval;
       std::string sval;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_FloatLitExpr(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!integer(ival)) return false;
            *result = new IntLit(ival);
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!string_lit(sval)) return false;
            *result = new StrLit(std::move(sval));
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("true")) return false;
            *result = new BoolLit(true);
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("false")) return false;
            *result = new BoolLit(false);
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("little")) return false;
            *result = new EndianLit(Endianness::Little);
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("big")) return false;
            *result = new EndianLit(Endianness::Big);
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!keyword("EOI")) return false;
            *result = new EOI();
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_CallExpr(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!ident(name)) return false;
            auto* s = new Simple(std::move(name), std::nullopt, std::nullopt);
         s->loc = Loc();
         *result = new Ref(s);
         (*result)->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("(")) return false;
        skip();
        if (!parse_Expr_(result)) return false;
        skip();
        if (!match(")")) return false;
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

bool Parser::parse_FloatLitExpr(Expr* * result) {
    double fval;
    skip();
    if (!floating(fval)) return false;
    *result = new FloatLit(static_cast<float>(fval));
         (*result)->loc = Loc();
    return true;
}

bool Parser::parse_CallExpr(Expr* * result) {
    std::string name;
       std::vector<Expr*> args;
       Expr* arg;
    skip();
    if (!ident(name)) return false;
    SourceLoc loc = Loc();
    skip();
    if (!match("(")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Expr_(&arg)) return false;
            args.push_back(arg);
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_Expr_(&arg)) return false;
                    args.push_back(arg);
                    return true;
                }()) { restore(_m1); break; }
            }
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match(")")) return false;
    *result = new Call(std::move(name), std::move(args));
         (*result)->loc = loc;
    return true;
}


