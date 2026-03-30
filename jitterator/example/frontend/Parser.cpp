#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Calc();
}

void Parser::setup_skip() {
    set_whitespace([](char c) -> bool {
        return ((((c == 13) || (c == 10)) || (c == 9)) || (c == 32));
    });
}


bool Parser::parse_Calc() {
    calc::Expr* e;
    skip();
    if (!parse_Expr(&e)) return false;
    skip(); if (!at_end()) return false;
    ast = new calc::Program(e);
    return true;
}

bool Parser::parse_Expr(calc::Expr* * result) {
    skip();
    if (!parse_AddExpr(result)) return false;
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
        skip();
        if (!parse_Atom(result)) return false;
        }
    }
    return true;
}

bool Parser::parse_Atom(calc::Expr* * result) {
    int64_t n;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("$")) return false;
            skip();
            if (!integer(n)) return false;
            *result = new calc::SlotRef(n);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!integer(n)) return false;
            *result = new calc::IntLit(n);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("(")) return false;
        skip();
        if (!parse_Expr(result)) return false;
        skip();
        if (!match(")")) return false;
        }
        }
    }
    return true;
}


