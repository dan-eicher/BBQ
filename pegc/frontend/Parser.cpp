#include "Parser.h"


#include "GrammarAST.h"
using namespace PegGrammar;


void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Peg();
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
    set_comments({{"//", "\n", false}, {"/*", "*/", false}});
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

bool Parser::char_lit(peg::Span& out) {
    const char* _start = pos();
    if (!match("'")) return false;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            if (!match("\\")) return false;
            if (at_end()) return false;
            advance();
            return true;
        }()) {} else {
        restore(_m0);
        {
            auto _m1 = save();
            bool _ok1 = [&]() -> bool {
                if (!match("'")) return false;
                return true;
            }();
            restore(_m1);
            if (_ok1) return false;
        }
        if (at_end()) return false;
        advance();
        }
    }
    if (!match("'")) return false;
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

bool Parser::parse_Peg() {
    peg::Span name_span;
       peg::Span endname_span;
       std::string name;
       std::string endname;
       std::vector<Header*> headers;
       std::vector<CharsetDef*> charsets;
       std::vector<TokenDef*> tokens;
       std::vector<CommentDef*> comments;
       bool has_ignore = false;
       CharsetExpr* ignore_expr = nullptr;
       std::vector<Production*> productions;
    skip();
    if (!match("COMPILER")) return false;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_SemAction_(headers)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("CHARACTERS")) return false;
            for (;;) {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_CharsetDecl(charsets)) return false;
                    return true;
                }()) { restore(_m2); break; }
            }
            return true;
        }()) restore(_m1);
    }
    {
        auto _m3 = save();
        if (![&]() -> bool {
            skip();
            if (!match("TOKENS")) return false;
            for (;;) {
                auto _m4 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_TokenDecl(tokens)) return false;
                    return true;
                }()) { restore(_m4); break; }
            }
            return true;
        }()) restore(_m3);
    }
    for (;;) {
        auto _m5 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_CommentDecl(comments)) return false;
            return true;
        }()) { restore(_m5); break; }
    }
    {
        auto _m6 = save();
        if (![&]() -> bool {
            skip();
            if (!match("IGNORE")) return false;
            skip();
            if (!parse_CharsetExpr_(ignore_expr)) return false;
            has_ignore = true;
            return true;
        }()) restore(_m6);
    }
    skip();
    if (!match("PRODUCTIONS")) return false;
    for (;;) {
        auto _m7 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_ProductionDecl(productions)) return false;
            return true;
        }()) { restore(_m7); break; }
    }
    skip();
    if (!match("END")) return false;
    skip();
    if (!ident(endname_span)) return false;
    endname = endname_span.to_string();
    skip();
    if (!match(".")) return false;
    ast = new Grammar(std::move(name), std::move(headers),
           std::move(charsets), std::move(tokens), std::move(comments),
           has_ignore,
           ignore_expr ? std::optional<CharsetExpr*>(ignore_expr) : std::nullopt,
           std::move(productions));
       ast->loc = SourceLoc{nullptr, 1, 1};
    return true;
}

bool Parser::parse_SemAction_(std::vector<Header*>& headers) {
    std::string code;
    skip();
    if (!match("(.")) return false;
    if (!scan_to("." ")", code)) return false;
    auto* h = new Header(TrimSemAction(code));
       h->loc = Loc();
       headers.push_back(h);
    return true;
}

bool Parser::parse_CharsetDecl(std::vector<CharsetDef*>& defs) {
    peg::Span csname_span; std::string csname; CharsetExpr* body;
    skip();
    if (!ident(csname_span)) return false;
    csname = csname_span.to_string(); SourceLoc loc = Loc();
    skip();
    if (!match("=")) return false;
    skip();
    if (!parse_CharsetExpr_(body)) return false;
    skip();
    if (!match(".")) return false;
    auto* d = new CharsetDef(std::move(csname), body);
       d->loc = loc;
       defs.push_back(d);
    return true;
}

bool Parser::parse_CharsetExpr_(CharsetExpr*& result) {
    CharsetExpr* rhs;
    skip();
    if (!parse_CharsetDiff(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("+")) return false;
            skip();
            if (!parse_CharsetDiff(rhs)) return false;
            auto* u = new Union(result, rhs); u->loc = result->loc; result = u;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_CharsetDiff(CharsetExpr*& result) {
    CharsetExpr* rhs;
    skip();
    if (!parse_CharsetPrimary(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("-")) return false;
            skip();
            if (!parse_CharsetPrimary(rhs)) return false;
            auto* d = new Diff(result, rhs); d->loc = result->loc; result = d;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_CharsetPrimary(CharsetExpr*& result) {
    peg::Span ch_span; peg::Span csname_span;
       char ch1 = 0, ch2 = 0; std::string csname;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!char_lit(ch_span)) return false;
            ch1 = peg::unescape_char_lit(ch_span);
                           result = new Single(static_cast<int>(ch1));
                           result->loc = Loc();
            {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("..")) return false;
                    skip();
                    if (!char_lit(ch_span)) return false;
                    ch2 = peg::unescape_char_lit(ch_span);
                             result = new Range(static_cast<int>(ch1), static_cast<int>(ch2));
                             result->loc = Loc();
                    return true;
                }()) restore(_m1);
            }
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("ANY")) return false;
            result = new Any(); result->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(csname_span)) return false;
        csname = csname_span.to_string();
                            result = new NameRef(std::move(csname));
                            result->loc = Loc();
        }
        }
    }
    return true;
}

bool Parser::parse_TokenDecl(std::vector<TokenDef*>& defs) {
    peg::Span tname_span; std::string tname; PegExpr* body;
    skip();
    if (!ident(tname_span)) return false;
    tname = tname_span.to_string(); SourceLoc loc = Loc();
    skip();
    if (!match("=")) return false;
    skip();
    if (!parse_PegBody(body)) return false;
    skip();
    if (!match(".")) return false;
    auto* d = new TokenDef(std::move(tname), body);
       d->loc = loc;
       defs.push_back(d);
    return true;
}

bool Parser::parse_CommentDecl(std::vector<CommentDef*>& defs) {
    peg::Span open_span; peg::Span close_span; peg::Span tname_span;
       bool nested = false; std::string open; std::string close_str; std::string tname;
    skip();
    if (!match("COMMENTS")) return false;
    skip();
    if (!match("FROM")) return false;
    skip();
    if (!string_lit(open_span)) return false;
    open = peg::unescape_string_lit(open_span);
                              SourceLoc loc = Loc();
    skip();
    if (!match("TO")) return false;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!string_lit(close_span)) return false;
            close_str = peg::unescape_string_lit(close_span);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(tname_span)) return false;
        close_str = tname_span.to_string();
        }
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("NESTED")) return false;
            nested = true;
            return true;
        }()) restore(_m1);
    }
    auto* c = new CommentDef(std::move(open), std::move(close_str), nested);
       c->loc = loc;
       defs.push_back(c);
    return true;
}

bool Parser::parse_ProductionDecl(std::vector<Production*>& prods) {
    peg::Span pname_span;
       std::string pname;
       std::vector<Param*> params;
       std::string locals;
       std::string raw;
       bool has_locals = false;
       PegExpr* body;
    skip();
    if (!ident(pname_span)) return false;
    pname = pname_span.to_string(); SourceLoc loc = Loc();
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("<.")) return false;
            if (!scan_to("." ">", raw)) return false;
            ParseParams(raw, params);
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", locals)) return false;
            locals = TrimSemAction(locals); has_locals = true;
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!match("=")) return false;
    skip();
    if (!parse_PegBody(body)) return false;
    skip();
    if (!match(".")) return false;
    auto* p = new Production(std::move(pname), std::move(params),
           has_locals ? std::optional<std::string>(std::move(locals)) : std::nullopt,
           body);
       p->loc = loc;
       prods.push_back(p);
    return true;
}

bool Parser::parse_PegBody(PegExpr*& result) {
    PegExpr* alt;
       std::vector<PegExpr*> alts;
    skip();
    if (!parse_PegSeq(result)) return false;
    alts.push_back(result);
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("|")) return false;
            skip();
            if (!parse_PegSeq(alt)) return false;
            alts.push_back(alt);
            return true;
        }()) { restore(_m0); break; }
    }
    if (alts.size() > 1) {
           auto loc = result->loc;
           result = new Choice(std::move(alts));
           result->loc = loc;
       }
    return true;
}

bool Parser::parse_PegSeq(PegExpr*& result) {
    PegExpr* elem;
       std::vector<PegExpr*> elems;
    skip();
    if (!parse_PegFactor(elem)) return false;
    elems.push_back(elem);
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_PegFactor(elem)) return false;
            elems.push_back(elem);
            return true;
        }()) { restore(_m0); break; }
    }
    if (elems.size() == 1)
           result = elems[0];
       else {
           auto loc = elems[0]->loc;
           result = new Sequence(std::move(elems));
           result->loc = loc;
       }
    return true;
}

bool Parser::parse_PegFactor(PegExpr*& result) {
    std::string code; PegExpr* inner;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("&")) return false;
            skip();
            if (!parse_PegAtom(inner)) return false;
            result = new And(inner);
         result->loc = inner->loc;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("!")) return false;
            skip();
            if (!parse_PegAtom(inner)) return false;
            result = new Not(inner);
         result->loc = inner->loc;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_PegAtom(result)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", code)) return false;
            result = new Action(TrimSemAction(code));
         result->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("IF")) return false;
        skip();
        if (!match("(")) return false;
        skip();
        if (!match("(.")) return false;
        if (!scan_to("." ")", code)) return false;
        skip();
        if (!match(")")) return false;
        skip();
        if (!parse_PegAtom(inner)) return false;
        result = new Resolver(TrimSemAction(code), inner);
         result->loc = inner->loc;
        }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_PegAtom(PegExpr*& result) {
    peg::Span s_span; peg::Span ch_span; peg::Span name_span;
       PegExpr* inner; std::string s; char ch1 = 0, ch2 = 0;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!string_lit(s_span)) return false;
            s = peg::unescape_string_lit(s_span);
         result = new Literal(std::move(s));
         result->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!char_lit(ch_span)) return false;
            ch1 = peg::unescape_char_lit(ch_span);
         result = new Literal(std::string(1, ch1));
         result->loc = Loc();
            {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("..")) return false;
                    skip();
                    if (!char_lit(ch_span)) return false;
                    ch2 = peg::unescape_char_lit(ch_span);
           result = new CharRange(static_cast<int>(ch1), static_cast<int>(ch2));
           result->loc = Loc();
                    return true;
                }()) restore(_m1);
            }
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("ANY")) return false;
            result = new AnyExpr();
         result->loc = Loc();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!ident(name_span)) return false;
            std::string rname = name_span.to_string(); SourceLoc loc = Loc();
         std::string args;
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (peek_at("<.")) {
                        skip();
                        if (!match("<.")) return false;
                        if (!scan_to("." ">", args)) return false;
           size_t b = args.find_first_not_of(" \t\r\n");
           size_t e = args.find_last_not_of(" \t\r\n");
           args = (b == std::string::npos) ? "" : args.substr(b, e - b + 1);
                    } else {
                        skip();
                        if (!match("<")) return false;
                        if (!scan_to(">", args)) return false;
           size_t b = args.find_first_not_of(" \t\r\n");
           size_t e = args.find_last_not_of(" \t\r\n");
           args = (b == std::string::npos) ? "" : args.substr(b, e - b + 1);
                    }
                    return true;
                }()) restore(_m2);
            }
            result = new RuleCall(std::move(rname), std::move(args));
         result->loc = loc;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_PegBody(inner)) return false;
            skip();
            if (!match(")")) return false;
            result = new Group(inner);
         result->loc = inner->loc;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("[")) return false;
            skip();
            if (!parse_PegBody(inner)) return false;
            skip();
            if (!match("]")) return false;
            result = new Optional(inner);
         result->loc = inner->loc;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("{")) return false;
        skip();
        if (!parse_PegBody(inner)) return false;
        bool is_plus = false;
        skip();
        if (!match("}")) return false;
        {
            auto _m3 = save();
            if (![&]() -> bool {
                skip();
                if (!match("+")) return false;
                is_plus = true;
                return true;
            }()) restore(_m3);
        }
        if (is_plus) {
             result = new Plus(inner);
         } else {
             result = new Star(inner);
         }
         result->loc = inner->loc;
        }
        }
        }
        }
        }
        }
    }
    return true;
}


