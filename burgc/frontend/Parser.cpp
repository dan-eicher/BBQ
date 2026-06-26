#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Burg();
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

bool Parser::qualified_ident(peg::Span& out) {
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
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            if (!match(":")) return false;
            if (!match(":")) return false;
            if (at_end() || !is_letter(peek())) return false;
            advance();
            for (;;) {
                auto _m2 = save();
                if (![&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }()) { restore(_m2); break; }
            }
            return true;
        }()) { restore(_m1); break; }
    }
    out.ptr = _start; out.len = static_cast<int>(pos() - _start);
    return true;
}

bool Parser::parse_Burg() {
    std::vector<burg_ast::TermDecl*> terms;
       std::string start_sym;
       std::string ns;
       std::string members_block;
       std::string private_block;
       std::vector<std::string> headers;
       std::vector<burg_ast::Rule*> rules;
       burg_ast::Rule* r;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Decl(terms, start_sym, ns, members_block, private_block, headers)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match("RULES")) return false;
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_RuleDecl(&r)) return false;
            rules.push_back(r);
            return true;
        }()) { restore(_m1); break; }
    }
    skip(); if (!at_end()) return false;
    ast = new burg_ast::Spec();
       ast->terminals = std::move(terms);
       ast->start = std::move(start_sym);
       ast->ns = std::move(ns);
       ast->members = std::move(members_block);
       ast->private_helpers = std::move(private_block);
       ast->headers = std::move(headers);
       ast->rules = std::move(rules);
       ast->loc = {1, 1};
    return true;
}

bool Parser::parse_Decl(std::vector<burg_ast::TermDecl*>& terms, std::string& start, std::string& ns, std::string& members, std::string& priv, std::vector<std::string>& headers) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_TermDecl_(terms)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_StartDecl(start)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_NamespaceDecl(ns)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_MembersDecl(members)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_PrivateDecl(priv)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_HeaderAction_(headers)) return false;
        }
        }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_NamespaceDecl(std::string& ns) {
    peg::Span ns_span;
    skip();
    if (!match("COMPILER")) return false;
    skip();
    if (!ident(ns_span)) return false;
    ns = ns_span.to_string();
    return true;
}

bool Parser::parse_MembersDecl(std::string& members) {
    std::string code;
    skip();
    if (!match("MEMBERS")) return false;
    skip();
    if (!match("(.")) return false;
    if (!scan_to("." ")", code)) return false;
    members = code;
    return true;
}

bool Parser::parse_PrivateDecl(std::string& priv) {
    std::string code;
    skip();
    if (!match("PRIVATE")) return false;
    skip();
    if (!match("(.")) return false;
    if (!scan_to("." ")", code)) return false;
    priv = code;
    return true;
}

bool Parser::parse_HeaderAction_(std::vector<std::string>& headers) {
    std::string code;
    skip();
    if (!match("(.")) return false;
    if (!scan_to("." ")", code)) return false;
    headers.push_back(code);
    return true;
}

bool Parser::parse_TermDecl_(std::vector<burg_ast::TermDecl*>& terms) {
    peg::Span name_span; peg::Span num_span; peg::Span sym_span;
       std::string name; int64_t num = 0;
       std::string sym; bool is_sym = false;
    skip();
    if (!match("TERM")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string();
            skip();
            if (!match("=")) return false;
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!integer(num_span)) return false;
                    num = std::stoll(num_span.to_string());
                                       is_sym = false;
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!qualified_ident(sym_span)) return false;
                sym = sym_span.to_string();
                                       is_sym = true;
                }
            }
            auto* t = new burg_ast::TermDecl();
           t->name = std::move(name);
           if (is_sym) t->symbol = std::move(sym);
           else        t->number = num;
           t->loc = {line(), col()};
           terms.push_back(t);
           name.clear(); sym.clear(); num = 0; is_sym = false;
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_StartDecl(std::string& start) {
    peg::Span start_span;
    skip();
    if (!match("START")) return false;
    skip();
    if (!ident(start_span)) return false;
    start = start_span.to_string();
    return true;
}

bool Parser::parse_RuleDecl(burg_ast::Rule* * result) {
    peg::Span nt_span; peg::Span cost_span;
       std::string nt;
       burg_ast::TreePattern* pat;
       int64_t cost = 0;
       std::string guard;
       std::string action;
    skip();
    if (!ident(nt_span)) return false;
    nt = nt_span.to_string();
                        burg_ast::SourceLoc loc = {line(), col()};
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TreePat(&pat)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("where")) return false;
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", guard)) return false;
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match("=")) return false;
    skip();
    if (!integer(cost_span)) return false;
    cost = std::stoll(cost_span.to_string());
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", action)) return false;
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!match(";")) return false;
    *result = new burg_ast::Rule();
       (*result)->nonterm = std::move(nt);
       (*result)->pattern = pat;
       (*result)->cost = cost;
       (*result)->guard = std::move(guard);
       (*result)->action = std::move(action);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_TreePat(burg_ast::TreePattern* * result) {
    peg::Span name_span;
       std::string name;
       std::vector<burg_ast::TreePattern*> children;
       burg_ast::TreePattern* child;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
                          burg_ast::SourceLoc loc = {line(), col()};
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_TreePat(&child)) return false;
            children.push_back(child);
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_TreePat(&child)) return false;
                    children.push_back(child);
                    return true;
                }()) { restore(_m1); break; }
            }
            skip();
            if (!match(")")) return false;
            return true;
        }()) restore(_m0);
    }
    *result = new burg_ast::TreePattern();
       (*result)->name = std::move(name);
       (*result)->children = std::move(children);
       (*result)->loc = loc;
    return true;
}


