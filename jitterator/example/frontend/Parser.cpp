#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Calc();
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

bool Parser::parse_Calc() {
    std::vector<calc::Function*> fns;
       calc::Expr* e;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_FunctionDecl(&fns)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!parse_Expr(&e)) return false;
    skip(); if (!at_end()) return false;
    ast = new calc::Program(std::move(fns), e);
    return true;
}

bool Parser::parse_FunctionDecl(std::vector<calc::Function*>* fns) {
    peg::Span name_span;
       std::vector<std::string> params;
       calc::Expr* body;
    skip();
    if (!match("fn")) return false;
    skip();
    if (!ident(name_span)) return false;
    skip();
    if (!match("(")) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Param(&params)) return false;
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_Param(&params)) return false;
                    return true;
                }()) { restore(_m1); break; }
            }
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!match(")")) return false;
    skip();
    if (!parse_Atom(&body)) return false;
    {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!match(";")) return false;
            return true;
        }()) restore(_m2);
    }
    fns->push_back(new calc::Function(
             name_span.to_string(), std::move(params), body));
    return true;
}

bool Parser::parse_Param(std::vector<std::string>* params) {
    peg::Span p_span;
    skip();
    if (!match("$")) return false;
    skip();
    if (!ident(p_span)) return false;
    params->push_back("$" + p_span.to_string());
    return true;
}

bool Parser::parse_Expr(calc::Expr* * result) {
    skip();
    if (!parse_SeqExpr(result)) return false;
    return true;
}

bool Parser::parse_SeqExpr(calc::Expr* * result) {
    calc::Expr* rest;
    skip();
    if (!parse_OrExpr(result)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match(";")) return false;
            skip();
            if (!parse_SeqExpr(&rest)) return false;
            *result = new calc::Sequence(*result, rest);
            return true;
        }()) restore(_m0);
    }
    return true;
}

bool Parser::parse_OrExpr(calc::Expr* * result) {
    calc::Expr* rhs;
    skip();
    if (!parse_AndExpr(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("||")) return false;
            skip();
            if (!parse_AndExpr(&rhs)) return false;
            *result = new calc::Or(*result, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_AndExpr(calc::Expr* * result) {
    calc::Expr* rhs;
    skip();
    if (!parse_CmpExpr(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("&&")) return false;
            skip();
            if (!parse_CmpExpr(&rhs)) return false;
            *result = new calc::And(*result, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_CmpExpr(calc::Expr* * result) {
    calc::Expr* rhs;
    skip();
    if (!parse_AddExpr(result)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!match("==")) return false;
                    skip();
                    if (!parse_AddExpr(&rhs)) return false;
                    *result = new calc::CmpEq(*result, rhs);
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match("!=")) return false;
                    skip();
                    if (!parse_AddExpr(&rhs)) return false;
                    *result = new calc::CmpNeq(*result, rhs);
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match("<=")) return false;
                    skip();
                    if (!parse_AddExpr(&rhs)) return false;
                    *result = new calc::CmpLe(*result, rhs);
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match(">=")) return false;
                    skip();
                    if (!parse_AddExpr(&rhs)) return false;
                    *result = new calc::CmpGe(*result, rhs);
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match("<")) return false;
                    skip();
                    if (!parse_AddExpr(&rhs)) return false;
                    *result = new calc::CmpLt(*result, rhs);
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!match(">")) return false;
                skip();
                if (!parse_AddExpr(&rhs)) return false;
                *result = new calc::CmpGt(*result, rhs);
                }
                }
                }
                }
                }
            }
            return true;
        }()) restore(_m0);
    }
    return true;
}

bool Parser::parse_AddExpr(calc::Expr* * result) {
    calc::Expr* right;
    skip();
    if (!parse_MulExpr(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("+")) {
                skip();
                if (!match("+")) return false;
                skip();
                if (!parse_MulExpr(&right)) return false;
                *result = new calc::BinOp(calc::Op::Add, *result, right);
            } else {
                skip();
                if (!match("-")) return false;
                skip();
                if (!parse_MulExpr(&right)) return false;
                *result = new calc::BinOp(calc::Op::Sub, *result, right);
            }
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_MulExpr(calc::Expr* * result) {
    calc::Expr* right;
    skip();
    if (!parse_UnaryExpr(result)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("*")) return false;
            skip();
            if (!parse_UnaryExpr(&right)) return false;
            *result = new calc::BinOp(calc::Op::Mul, *result, right);
            return true;
        }()) { restore(_m0); break; }
    }
    return true;
}

bool Parser::parse_UnaryExpr(calc::Expr* * result) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("-")) return false;
            skip();
            if (!parse_UnaryExpr(result)) return false;
            *result = new calc::NegOp(*result);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("!")) return false;
            skip();
            if (!parse_UnaryExpr(result)) return false;
            *result = new calc::Not(*result);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_Atom(result)) return false;
        }
        }
    }
    return true;
}

bool Parser::parse_Atom(calc::Expr* * result) {
    peg::Span n_span; peg::Span name_span; int64_t n;
       calc::Expr* val = nullptr;
       calc::Expr* cond = nullptr; calc::Expr* then_ = nullptr; calc::Expr* else_ = nullptr;
       calc::Expr* body = nullptr;
       std::vector<calc::Expr*> args;
       calc::Expr* arg = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("let")) return false;
            skip();
            if (!match("$")) return false;
            skip();
            if (!ident(name_span)) return false;
            skip();
            if (!match("=")) return false;
            skip();
            if (!parse_AddExpr(&val)) return false;
            *result = new calc::LetDecl("$" + name_span.to_string(), val);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("$")) return false;
            skip();
            if (!ident(name_span)) return false;
            *result = new calc::Var("$" + name_span.to_string());
            {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("=")) return false;
                    skip();
                    if (!parse_AddExpr(&val)) return false;
                    *result = new calc::VarAssign("$" + name_span.to_string(), val);
                    return true;
                }()) restore(_m1);
            }
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!integer(n_span)) return false;
            n = std::stoll(n_span.to_string());
                             *result = new calc::IntLit(n);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr(result)) return false;
            skip();
            if (!match(")")) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("if")) return false;
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr(&cond)) return false;
            skip();
            if (!match(")")) return false;
            skip();
            if (!parse_CmpExpr(&then_)) return false;
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("else")) return false;
                    skip();
                    if (!parse_CmpExpr(&else_)) return false;
                    return true;
                }()) restore(_m2);
            }
            *result = new calc::If(cond, then_, else_);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("while")) return false;
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_Expr(&cond)) return false;
            skip();
            if (!match(")")) return false;
            skip();
            if (!parse_CmpExpr(&body)) return false;
            *result = new calc::While(cond, body);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("break")) return false;
            *result = new calc::Break();
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("return")) return false;
            skip();
            if (!parse_CmpExpr(&val)) return false;
            *result = new calc::Return(val);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(name_span)) return false;
        skip();
        if (!match("(")) return false;
        {
            auto _m3 = save();
            if (![&]() -> bool {
                skip();
                if (!parse_Expr(&arg)) return false;
                args.push_back(arg);
                for (;;) {
                    auto _m4 = save();
                    if (![&]() -> bool {
                        skip();
                        if (!match(",")) return false;
                        skip();
                        if (!parse_Expr(&arg)) return false;
                        args.push_back(arg);
                        return true;
                    }()) { restore(_m4); break; }
                }
                return true;
            }()) restore(_m3);
        }
        skip();
        if (!match(")")) return false;
        *result = new calc::Call(name_span.to_string(), std::move(args));
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


