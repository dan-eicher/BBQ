#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Burg();
}

void Parser::setup_skip() {
    set_whitespace([](char c) -> bool {
        return ((((c == 13) || (c == 10)) || (c == 9)) || (c == 32));
    });
    set_comments({{"//", "\n", false}, {"/*", "*/", false}});
}


bool Parser::parse_Burg() {
    std::vector<burg_ast::TermDecl*> terms;
       std::string start_sym;
       std::string members_block;
       std::vector<std::string> headers;
       std::vector<burg_ast::Rule*> rules;
       burg_ast::Rule* r;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Decl(terms, start_sym, members_block, headers)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!keyword("RULES")) return false;
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
       ast->members = std::move(members_block);
       ast->headers = std::move(headers);
       ast->rules = std::move(rules);
       ast->loc = {1, 1};
    return true;
}

bool Parser::parse_Decl(std::vector<burg_ast::TermDecl*>& terms, std::string& start, std::string& members, std::vector<std::string>& headers) {
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
            if (!parse_MembersDecl(members)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_HeaderAction_(headers)) return false;
        }
        }
        }
    }
    return true;
}

bool Parser::parse_MembersDecl(std::string& members) {
    std::string code;
    skip();
    if (!keyword("MEMBERS")) return false;
    skip();
    if (!match("(.")) return false;
    if (!scan_to("." ")", code)) return false;
    members = code;
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
    std::string name; int64_t num;
    skip();
    if (!keyword("TERM")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!ident(name)) return false;
            skip();
            if (!match("=")) return false;
            skip();
            if (!integer(num)) return false;
            auto* t = new burg_ast::TermDecl();
           t->name = std::move(name);
           t->number = num;
           t->loc = {line(), col()};
           terms.push_back(t);
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_StartDecl(std::string& start) {
    skip();
    if (!keyword("START")) return false;
    skip();
    if (!ident(start)) return false;
    return true;
}

bool Parser::parse_RuleDecl(burg_ast::Rule* * result) {
    std::string nt;
       burg_ast::TreePattern* pat;
       int64_t rulenum;
       int64_t cost = 0;
       std::string guard;
       std::string action;
    skip();
    if (!ident(nt)) return false;
    burg_ast::SourceLoc loc = {line(), col()};
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_TreePat(&pat)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!keyword("where")) return false;
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", guard)) return false;
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match("=")) return false;
    skip();
    if (!integer(rulenum)) return false;
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!integer(cost)) return false;
            skip();
            if (!match(")")) return false;
            return true;
        }()) restore(_m1);
    }
    {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", action)) return false;
            return true;
        }()) restore(_m2);
    }
    skip();
    if (!match(";")) return false;
    *result = new burg_ast::Rule();
       (*result)->nonterm = std::move(nt);
       (*result)->pattern = pat;
       (*result)->rule_number = rulenum;
       (*result)->cost = cost;
       (*result)->guard = std::move(guard);
       (*result)->action = std::move(action);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_TreePat(burg_ast::TreePattern* * result) {
    std::string name;
       std::vector<burg_ast::TreePattern*> children;
       burg_ast::TreePattern* child;
    skip();
    if (!ident(name)) return false;
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


