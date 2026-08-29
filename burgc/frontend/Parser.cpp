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
       std::vector<std::string> entry_syms;
       std::string ns;
       std::string members_block;
       std::string private_block;
       std::vector<std::string> headers;
       std::vector<burg_ast::Rule*> rules;
       std::vector<burg_ast::RewriteRule*> rewrites;
       std::vector<burg_ast::AnalysisDecl*> analyses;
       std::vector<burg_ast::AuxDecl*> auxes;
       burg_ast::Rule* r;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Decl(terms, start_sym, entry_syms, ns, members_block, private_block, headers, auxes, rewrites, analyses)) return false;
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
       ast->entries = std::move(entry_syms);
       ast->ns = std::move(ns);
       ast->members = std::move(members_block);
       ast->private_helpers = std::move(private_block);
       ast->headers = std::move(headers);
       ast->rules = std::move(rules);
       ast->auxiliaries = std::move(auxes);
       ast->rewrites = std::move(rewrites);
       ast->analyses = std::move(analyses);
       ast->loc = {1, 1};
    return true;
}

bool Parser::parse_Decl(std::vector<burg_ast::TermDecl*>& terms, std::string& start, std::vector<std::string>& entries, std::string& ns, std::string& members, std::string& priv, std::vector<std::string>& headers, std::vector<burg_ast::AuxDecl*>& auxes, std::vector<burg_ast::RewriteRule*>& rewrites, std::vector<burg_ast::AnalysisDecl*>& analyses) {
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
            if (!parse_AuxSection(auxes)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_RewriteSection(rewrites, analyses)) return false;
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
            if (!parse_EntryDecl(entries)) return false;
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

bool Parser::parse_EntryDecl(std::vector<std::string>& entries) {
    peg::Span entry_span;
    skip();
    if (!match("ENTRY")) return false;
    skip();
    if (!ident(entry_span)) return false;
    entries.push_back(entry_span.to_string());
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

bool Parser::parse_AuxSection(std::vector<burg_ast::AuxDecl*>& out) {
    burg_ast::AuxDecl* ad;
    skip();
    if (!match("AUXILIARIES")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_AuxDecl_(&ad)) return false;
            out.push_back(ad);
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_AuxDecl_(burg_ast::AuxDecl* * result) {
    peg::Span name_span; peg::Span ty_span;
       std::string name; std::string ret;
       std::vector<std::string> params;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
                          burg_ast::SourceLoc loc = {line(), col()};
    skip();
    if (!match(":")) return false;
    skip();
    if (!match("(")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!ident(ty_span)) return false;
            params.push_back(ty_span.to_string());
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!ident(ty_span)) return false;
                    params.push_back(ty_span.to_string());
                    return true;
                }()) { restore(_m1); break; }
            }
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match(")")) return false;
    skip();
    if (!match("->")) return false;
    skip();
    if (!ident(ty_span)) return false;
    ret = ty_span.to_string();
    *result = new burg_ast::AuxDecl();
       (*result)->name = std::move(name);
       (*result)->params = std::move(params);
       (*result)->ret = std::move(ret);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_RewriteSection(std::vector<burg_ast::RewriteRule*>& out, std::vector<burg_ast::AnalysisDecl*>& analyses) {
    burg_ast::RewriteRule* rr; burg_ast::AnalysisDecl* ad;
    skip();
    if (!match("REWRITE")) return false;
    skip();
    if (!match("{")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!parse_AnalysisDecl_(&ad)) return false;
                    analyses.push_back(ad);
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!parse_RewriteRule_(&rr)) return false;
                out.push_back(rr);
                }
            }
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match("}")) return false;
    return true;
}

bool Parser::parse_AnalysisDecl_(burg_ast::AnalysisDecl* * result) {
    peg::Span ty_span; peg::Span key_span; peg::Span fn_span;
       std::string fact; std::string key;
       std::string make_fn; std::string join_fn; std::string modify_fn;
       std::vector<std::string> unknown;
    skip();
    if (!match("ANALYSIS")) return false;
    skip();
    if (!ident(ty_span)) return false;
    fact = ty_span.to_string();
                        burg_ast::SourceLoc loc = {line(), col()};
    skip();
    if (!match("{")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!ident(key_span)) return false;
            key = key_span.to_string();
            skip();
            if (!match(":")) return false;
            skip();
            if (!ident(fn_span)) return false;
            if      (key == "make")   make_fn   = fn_span.to_string();
                            else if (key == "join")   join_fn   = fn_span.to_string();
                            else if (key == "modify") modify_fn = fn_span.to_string();
                            else                      unknown.push_back(key);
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match("}")) return false;
    *result = new burg_ast::AnalysisDecl();
       (*result)->fact = std::move(fact);
       (*result)->make = std::move(make_fn);
       (*result)->join = std::move(join_fn);
       (*result)->modify = std::move(modify_fn);
       (*result)->unknown_hooks = std::move(unknown);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_RewriteRule_(burg_ast::RewriteRule* * result) {
    peg::Span name_span;
       std::string name;
       burg_ast::RewritePat* pat;
       burg_ast::RewriteTmpl* tmpl;
       std::string guard;
    skip();
    if (!ident(name_span)) return false;
    name = name_span.to_string();
                          burg_ast::SourceLoc loc = {line(), col()};
    skip();
    if (!match(":")) return false;
    skip();
    if (!parse_RewritePat_(&pat)) return false;
    skip();
    if (!match("=>")) return false;
    skip();
    if (!parse_RewriteTmpl_(&tmpl)) return false;
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
    if (!match(";")) return false;
    *result = new burg_ast::RewriteRule();
       (*result)->name = std::move(name);
       (*result)->pattern = pat;
       (*result)->tmpl = tmpl;
       (*result)->guard = std::move(guard);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_RewritePat_(burg_ast::RewritePat* * result) {
    peg::Span name_span;
       std::string name;
       bool is_binder = false;
       std::vector<burg_ast::RewritePat*> children;
       burg_ast::RewritePat* child;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("$")) return false;
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string(); is_binder = true;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(name_span)) return false;
        name = name_span.to_string();
        {
            auto _m1 = save();
            if (![&]() -> bool {
                skip();
                if (!match("(")) return false;
                skip();
                if (!parse_RewritePat_(&child)) return false;
                children.push_back(child);
                for (;;) {
                    auto _m2 = save();
                    if (![&]() -> bool {
                        skip();
                        if (!match(",")) return false;
                        skip();
                        if (!parse_RewritePat_(&child)) return false;
                        children.push_back(child);
                        return true;
                    }()) { restore(_m2); break; }
                }
                skip();
                if (!match(")")) return false;
                return true;
            }()) restore(_m1);
        }
        }
    }
    burg_ast::SourceLoc loc = {line(), col()};
       *result = new burg_ast::RewritePat();
       (*result)->name = std::move(name);
       (*result)->is_binder = is_binder;
       (*result)->children = std::move(children);
       (*result)->loc = loc;
    return true;
}

bool Parser::parse_RewriteTmpl_(burg_ast::RewriteTmpl* * result) {
    peg::Span name_span;
       std::string name;
       burg_ast::RewriteTmpl::Kind kind = burg_ast::RewriteTmpl::Term;
       std::vector<burg_ast::RewriteTmpl*> children;
       burg_ast::RewriteTmpl* child;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("$")) return false;
            skip();
            if (!match("$")) return false;
            name = "$"; kind = burg_ast::RewriteTmpl::Binder;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("$")) return false;
            skip();
            if (!ident(name_span)) return false;
            name = name_span.to_string(); kind = burg_ast::RewriteTmpl::Binder;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(name_span)) return false;
        name = name_span.to_string();
        {
            auto _m1 = save();
            if (![&]() -> bool {
                skip();
                if (!match("(")) return false;
                skip();
                if (!parse_RewriteTmpl_(&child)) return false;
                children.push_back(child);
                for (;;) {
                    auto _m2 = save();
                    if (![&]() -> bool {
                        skip();
                        if (!match(",")) return false;
                        skip();
                        if (!parse_RewriteTmpl_(&child)) return false;
                        children.push_back(child);
                        return true;
                    }()) { restore(_m2); break; }
                }
                skip();
                if (!match(")")) return false;
                return true;
            }()) restore(_m1);
        }
        }
        }
    }
    burg_ast::SourceLoc loc = {line(), col()};
       *result = new burg_ast::RewriteTmpl();
       (*result)->kind = kind;
       (*result)->name = std::move(name);
       (*result)->children = std::move(children);
       (*result)->loc = loc;
    return true;
}


