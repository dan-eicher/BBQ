#include "Parser.h"



void Parser::init(const char* input, int length) {
    PegCursor::init(input, length);
    setup_skip();
}

bool Parser::parse() {
    skip();
    return parse_Opgen();
}

static bool is_letter(char c) {
    return ((((unsigned char)c >= 97 && (unsigned char)c <= 122) || ((unsigned char)c >= 65 && (unsigned char)c <= 90)) || ((unsigned char)c == 95));
}

static bool is_digit(char c) {
    return ((unsigned char)c >= 48 && (unsigned char)c <= 57);
}

static bool is_hex_digit(char c) {
    return ((is_digit(c) || ((unsigned char)c >= 97 && (unsigned char)c <= 102)) || ((unsigned char)c >= 65 && (unsigned char)c <= 70));
}

static bool is_ident_continue(char c) {
    return (is_letter(c) || is_digit(c));
}

void Parser::setup_skip() {
    set_whitespace([](char c) -> bool {
        return (((((unsigned char)c == 13) || ((unsigned char)c == 10)) || ((unsigned char)c == 9)) || ((unsigned char)c == 32));
    });
    set_comments({{"#", "\n", false, false}});
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
    {
        auto _m0 = save();
        if ([&]() -> bool {
            if (!match("0")) return false;
            if (peek_at("x")) {
                if (!match("x")) return false;
            } else {
                if (!match("X")) return false;
            }
            if (at_end() || !is_hex_digit(peek())) return false;
            advance();
            for (;;) {
                auto _m1 = save();
                if (![&]() -> bool {
                    if (at_end() || !is_hex_digit(peek())) return false;
                    advance();
                    return true;
                }()) { restore(_m1); break; }
            }
            return true;
        }()) {} else {
        restore(_m0);
        if (at_end() || !is_digit(peek())) return false;
        advance();
        for (;;) {
            auto _m2 = save();
            if (![&]() -> bool {
                if (at_end() || !is_digit(peek())) return false;
                advance();
                return true;
            }()) { restore(_m2); break; }
        }
        }
    }
    out.ptr = _start; out.len = static_cast<int>(pos() - _start);
    return true;
}

bool Parser::floating(peg::Span& out) {
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
    if (!match(".")) return false;
    if (at_end() || !is_digit(peek())) return false;
    advance();
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            if (at_end() || !is_digit(peek())) return false;
            advance();
            return true;
        }()) { restore(_m1); break; }
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
                {
                    auto _m3 = save();
                    bool _ok3 = [&]() -> bool {
                        if (!match("\\")) return false;
                        return true;
                    }();
                    restore(_m3);
                    if (_ok3) return false;
                }
                {
                    auto _m4 = save();
                    bool _ok4 = [&]() -> bool {
                        if (!match("\r")) return false;
                        return true;
                    }();
                    restore(_m4);
                    if (_ok4) return false;
                }
                {
                    auto _m5 = save();
                    bool _ok5 = [&]() -> bool {
                        if (!match("\n")) return false;
                        return true;
                    }();
                    restore(_m5);
                    if (_ok5) return false;
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

bool Parser::parse_Opgen() {
    std::vector<const char*> headers;
       std::vector<opgen::Opcode*> ops;
       opgen::Opcode* op = nullptr;
       std::vector<opgen::MethodDecl*> methods;
       std::optional<opgen::StatusDecl*> status;
       std::vector<opgen::TypeDecl*> types;
       std::vector<opgen::FactDecl*> fact_decls;
       std::optional<const char*> body_c;
       std::optional<const char*> backend;
       bool sidetable = false;
       std::string hdr;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", hdr)) return false;
           headers.push_back(opgen_intern(hdr));
            return true;
        }()) { restore(_m0); break; }
    }
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            {
                auto _m2 = save();
                if ([&]() -> bool {
                    skip();
                    if (!parse_StatusDecl(status)) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                if ([&]() -> bool {
                    skip();
                    if (!parse_BackendDecl(backend)) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                if ([&]() -> bool {
                    skip();
                    if (!parse_SidetableDecl(sidetable)) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                if ([&]() -> bool {
                    skip();
                    if (!parse_TypeDecl(types)) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                if ([&]() -> bool {
                    skip();
                    if (!parse_FactsDecl(fact_decls)) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                if ([&]() -> bool {
                    skip();
                    if (!parse_MethodDecl(methods)) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                if ([&]() -> bool {
                    skip();
                    if (!parse_FamilyDef(ops)) return false;
                    return true;
                }()) {} else {
                restore(_m2);
                skip();
                if (!parse_OpcodeDef(op)) return false;
                ops.push_back(op);
                }
                }
                }
                }
                }
                }
                }
            }
            return true;
        }()) { restore(_m1); break; }
    }
    {
        auto _m3 = save();
        if (![&]() -> bool {
            skip();
            if (!match("%%")) return false;
            /* Everything from here to EOF becomes `body_c`,
            * emitted verbatim at the end of opcode_tables.c. */
           const char* bstart = pos();
           while (!at_end()) advance();
           peg::Span bspan{ bstart, (int)(pos() - bstart) };
           body_c = opgen_dup(bspan);
            return true;
        }()) restore(_m3);
    }
    skip(); if (!at_end()) return false;
    ast = new opgen::Module(std::move(ops), std::move(methods), status,
           std::move(types), std::move(fact_decls),
           std::move(headers), body_c, backend, sidetable);
    return true;
}

bool Parser::parse_OpcodeDef(opgen::Opcode*& result) {
    peg::Span ms; peg::Span isp; int64_t opcode_val = 0;
       peg::Span ssp; int32_t subop = -1;
       const char* mnemonic = nullptr;
       std::vector<opgen::OperandParam*> operands;
       std::vector<opgen::StackParam*> stack_in, stack_out;
       std::vector<opgen::ErrorCase*> errors;
       std::vector<const char*> flags;
       std::optional<opgen::LocalValueDecl*> local_value;
       std::vector<opgen::VerifyRejectCase*> verify_rejects;
       std::vector<opgen::EdgeCase*> edges;
       std::optional<opgen::Branch*> br;
       std::vector<opgen::AnnotationFact*> facts;
       std::optional<opgen::LocalIndexDecl*> local_index;
       std::optional<opgen::SwitchPayloadDecl*> switch_payload;
       std::optional<opgen::NarrowFormDecl*> narrow_form;
       std::optional<opgen::SpecRef*> spec;
       std::optional<const char*> setup;
       std::optional<const char*> verify;
       std::vector<opgen::SemStmt*> sem_body;
    skip();
    if (!ident(ms)) return false;
    mnemonic = opgen_dup(ms);
    skip();
    if (!integer(isp)) return false;
    opcode_val = opgen_parse_int(isp);
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!integer(ssp)) return false;
            subop = (int32_t)opgen_parse_int(ssp);
            return true;
        }()) restore(_m0);
    }
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("[")) return false;
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_OperandParam(operands)) return false;
                    for (;;) {
                        auto _m3 = save();
                        if (![&]() -> bool {
                            skip();
                            if (!match(",")) return false;
                            skip();
                            if (!parse_OperandParam(operands)) return false;
                            return true;
                        }()) { restore(_m3); break; }
                    }
                    return true;
                }()) restore(_m2);
            }
            skip();
            if (!match("]")) return false;
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!match("(")) return false;
    {
        auto _m4 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_StackParam(stack_in)) return false;
            for (;;) {
                auto _m5 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_StackParam(stack_in)) return false;
                    return true;
                }()) { restore(_m5); break; }
            }
            return true;
        }()) restore(_m4);
    }
    skip();
    if (!match("--")) return false;
    {
        auto _m6 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_StackParam(stack_out)) return false;
            for (;;) {
                auto _m7 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_StackParam(stack_out)) return false;
                    return true;
                }()) { restore(_m7); break; }
            }
            return true;
        }()) restore(_m6);
    }
    skip();
    if (!match(")")) return false;
    for (;;) {
        auto _m8 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Annotation(errors, flags, local_value, verify_rejects, edges, br,
                   facts, local_index,
                   switch_payload, narrow_form, spec, setup, verify)) return false;
            return true;
        }()) { restore(_m8); break; }
    }
    {
        auto _m9 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_SemBody(sem_body)) return false;
            return true;
        }()) restore(_m9);
    }
    result = new opgen::Opcode(
             mnemonic, (int32_t)opcode_val, subop,
             std::move(operands), std::move(stack_in), std::move(stack_out),
             std::move(errors), std::move(flags), local_value,
             std::move(verify_rejects), std::move(edges), br,
             std::move(facts),
             local_index, switch_payload, narrow_form, spec, setup, verify,
             std::move(sem_body));
    return true;
}

bool Parser::parse_FamilyDef(std::vector<opgen::Opcode*>& ops) {
    std::vector<opgen::OperandParam*> operands;
       std::vector<opgen::StackParam*> tin, tout;
       std::vector<int> in_var, out_var;
       std::vector<opgen::ErrorCase*> errors;
       std::vector<const char*> flags;
       std::optional<opgen::LocalValueDecl*> local_value;
       std::vector<opgen::VerifyRejectCase*> verify_rejects;
       std::vector<opgen::EdgeCase*> edges;
       std::optional<opgen::Branch*> br;
       std::vector<opgen::AnnotationFact*> facts;
       std::optional<opgen::LocalIndexDecl*> local_index;
       std::optional<opgen::SwitchPayloadDecl*> switch_payload;
       std::optional<opgen::NarrowFormDecl*> narrow_form;
       std::optional<opgen::SpecRef*> spec;
       std::optional<const char*> setup;
       std::optional<const char*> verify;
       std::vector<opgen::SemStmt*> sem_body;
       std::vector<opgen::ValueType> mvts; std::vector<const char*> mnames;
       std::vector<int64_t> mhexes; std::vector<int64_t> msubs;
    skip();
    if (!match("family")) return false;
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
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("[")) return false;
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_OperandParam(operands)) return false;
                    for (;;) {
                        auto _m3 = save();
                        if (![&]() -> bool {
                            skip();
                            if (!match(",")) return false;
                            skip();
                            if (!parse_OperandParam(operands)) return false;
                            return true;
                        }()) { restore(_m3); break; }
                    }
                    return true;
                }()) restore(_m2);
            }
            skip();
            if (!match("]")) return false;
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!match("(")) return false;
    {
        auto _m4 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_FamilyStackParam(tin, in_var)) return false;
            for (;;) {
                auto _m5 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_FamilyStackParam(tin, in_var)) return false;
                    return true;
                }()) { restore(_m5); break; }
            }
            return true;
        }()) restore(_m4);
    }
    skip();
    if (!match("--")) return false;
    {
        auto _m6 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_FamilyStackParam(tout, out_var)) return false;
            for (;;) {
                auto _m7 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_FamilyStackParam(tout, out_var)) return false;
                    return true;
                }()) { restore(_m7); break; }
            }
            return true;
        }()) restore(_m6);
    }
    skip();
    if (!match(")")) return false;
    for (;;) {
        auto _m8 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_Annotation(errors, flags, local_value, verify_rejects, edges, br,
                   facts, local_index,
                   switch_payload, narrow_form, spec, setup, verify)) return false;
            return true;
        }()) { restore(_m8); break; }
    }
    {
        auto _m9 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_SemBody(sem_body)) return false;
            return true;
        }()) restore(_m9);
    }
    skip();
    if (!match("{")) return false;
    skip();
    if (!parse_FamilyMember(mvts, mnames, mhexes, msubs)) return false;
    for (;;) {
        auto _m10 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_FamilyMember(mvts, mnames, mhexes, msubs)) return false;
            return true;
        }()) { restore(_m10); break; }
    }
    skip();
    if (!match("}")) return false;
    for (size_t m = 0; m < mnames.size(); m++) {
             std::vector<opgen::StackParam*> nin;
             for (size_t i = 0; i < tin.size(); i++) {
                 opgen::ValueType ty = in_var[i] ? mvts[m] : tin[i]->ty;
                 nin.push_back(new opgen::StackParam(ty, tin[i]->name, tin[i]->sem_expr, tin[i]->count, tin[i]->at_src));
             }
             std::vector<opgen::StackParam*> nout;
             for (size_t i = 0; i < tout.size(); i++) {
                 opgen::ValueType ty = out_var[i] ? mvts[m] : tout[i]->ty;
                 nout.push_back(new opgen::StackParam(ty, tout[i]->name, tout[i]->sem_expr, tout[i]->count, tout[i]->at_src));
             }
             ops.push_back(new opgen::Opcode(
                 mnames[m], (int32_t)mhexes[m], (int32_t)msubs[m],
                 operands, std::move(nin), std::move(nout),
                 errors, flags, local_value,
                 verify_rejects, edges, br,
                 facts,
                 local_index, switch_payload, narrow_form, spec, setup, verify,
                 sem_body));
         }
    return true;
}

bool Parser::parse_FamilyStackParam(std::vector<opgen::StackParam*>& list, std::vector<int>& var_flags) {
    opgen::ValueType ty = opgen::ValueType::TyI32; peg::Span ns; peg::Span es;
       const char* sem_expr = nullptr; int isvar = 0;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("T")) return false;
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
            isvar = 1;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_ValueType(ty)) return false;
        }
    }
    skip();
    if (!ident(ns)) return false;
    {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!match("=")) return false;
            skip();
            if (!string_lit(es)) return false;
            sem_expr = opgen_dup(es);
            return true;
        }()) restore(_m2);
    }
    var_flags.push_back(isvar);
         list.push_back(new opgen::StackParam(ty, opgen_dup(ns), opgen_opt(sem_expr),
                                              opgen_opt((opgen::SemExpr*)nullptr),
                                              opgen_opt((opgen::SemExpr*)nullptr)));
    return true;
}

bool Parser::parse_FamilyMember(std::vector<opgen::ValueType>& mvts, std::vector<const char*>& mnames, std::vector<int64_t>& mhexes, std::vector<int64_t>& msubs) {
    opgen::ValueType ty; peg::Span ms; peg::Span hs; peg::Span hs2; int has_sub = 0;
    skip();
    if (!parse_ValueType(ty)) return false;
    skip();
    if (!ident(ms)) return false;
    skip();
    if (!integer(hs)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!integer(hs2)) return false;
            has_sub = 1;
            return true;
        }()) restore(_m0);
    }
    mvts.push_back(ty); mnames.push_back(opgen_dup(ms));
         mhexes.push_back(opgen_parse_int(hs));
         msubs.push_back(has_sub ? opgen_parse_int(hs2) : -1);
    return true;
}

bool Parser::parse_OperandParam(std::vector<opgen::OperandParam*>& list) {
    opgen::ValueType ty; peg::Span ns; bool cp_ref = false;
    skip();
    if (!parse_ValueType(ty)) return false;
    skip();
    if (!ident(ns)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("cp_ref")) return false;
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
            cp_ref = true;
            return true;
        }()) restore(_m0);
    }
    list.push_back(new opgen::OperandParam(ty, opgen_dup(ns), cp_ref));
    return true;
}

bool Parser::parse_StackParam(std::vector<opgen::StackParam*>& list) {
    opgen::ValueType ty; peg::Span ns; peg::Span es;
       const char* sem_expr = nullptr; opgen::SemExpr* count = nullptr; opgen::SemExpr* at_src = nullptr;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("[")) return false;
            skip();
            if (!parse_SemExpr(count)) return false;
            skip();
            if (!match("]")) return false;
            return true;
        }()) restore(_m0);
    }
    skip();
    if (!parse_ValueType(ty)) return false;
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_SemExpr(at_src)) return false;
            skip();
            if (!match(")")) return false;
            return true;
        }()) restore(_m1);
    }
    skip();
    if (!ident(ns)) return false;
    {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!match("=")) return false;
            skip();
            if (!string_lit(es)) return false;
            sem_expr = opgen_dup(es);
            return true;
        }()) restore(_m2);
    }
    list.push_back(new opgen::StackParam(ty, opgen_dup(ns), opgen_opt(sem_expr),
                                              opgen_opt(count), opgen_opt(at_src)));
    return true;
}

bool Parser::parse_ValueType(opgen::ValueType& out) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("dword")) return false;
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
            out = opgen::ValueType::TyDword;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("word")) return false;
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
            out = opgen::ValueType::TyWord;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("addr")) return false;
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
            out = opgen::ValueType::TyAddr;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("ref")) return false;
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
            out = opgen::ValueType::TyRef;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i64")) return false;
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
            out = opgen::ValueType::TyI64;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i32")) return false;
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
            out = opgen::ValueType::TyI32;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i16")) return false;
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
            out = opgen::ValueType::TyI16;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i8")) return false;
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
            out = opgen::ValueType::TyI8;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("u16")) return false;
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
            out = opgen::ValueType::TyU16;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("u8")) return false;
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
            out = opgen::ValueType::TyU8;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("f64")) return false;
            {
                auto _m11 = save();
                bool _ok11 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m11);
                if (_ok11) return false;
            }
            out = opgen::ValueType::TyF64;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("f32")) return false;
            {
                auto _m12 = save();
                bool _ok12 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m12);
                if (_ok12) return false;
            }
            out = opgen::ValueType::TyF32;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i8x16")) return false;
            {
                auto _m13 = save();
                bool _ok13 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m13);
                if (_ok13) return false;
            }
            out = opgen::ValueType::TyI8x16;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i16x8")) return false;
            {
                auto _m14 = save();
                bool _ok14 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m14);
                if (_ok14) return false;
            }
            out = opgen::ValueType::TyI16x8;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i32x4")) return false;
            {
                auto _m15 = save();
                bool _ok15 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m15);
                if (_ok15) return false;
            }
            out = opgen::ValueType::TyI32x4;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i64x2")) return false;
            {
                auto _m16 = save();
                bool _ok16 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m16);
                if (_ok16) return false;
            }
            out = opgen::ValueType::TyI64x2;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("f32x4")) return false;
            {
                auto _m17 = save();
                bool _ok17 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m17);
                if (_ok17) return false;
            }
            out = opgen::ValueType::TyF32x4;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("f64x2")) return false;
            {
                auto _m18 = save();
                bool _ok18 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m18);
                if (_ok18) return false;
            }
            out = opgen::ValueType::TyF64x2;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("v128")) return false;
            {
                auto _m19 = save();
                bool _ok19 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m19);
                if (_ok19) return false;
            }
            out = opgen::ValueType::TyV128;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("funcref")) return false;
            {
                auto _m20 = save();
                bool _ok20 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m20);
                if (_ok20) return false;
            }
            out = opgen::ValueType::TyFuncRef;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("externref")) return false;
            {
                auto _m21 = save();
                bool _ok21 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m21);
                if (_ok21) return false;
            }
            out = opgen::ValueType::TyExternRef;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("i31ref")) return false;
            {
                auto _m22 = save();
                bool _ok22 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m22);
                if (_ok22) return false;
            }
            out = opgen::ValueType::TyI31Ref;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("uleb128")) return false;
            {
                auto _m23 = save();
                bool _ok23 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m23);
                if (_ok23) return false;
            }
            out = opgen::ValueType::TyUleb32;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("uleb64")) return false;
            {
                auto _m24 = save();
                bool _ok24 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m24);
                if (_ok24) return false;
            }
            out = opgen::ValueType::TyUleb64;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("uleb32")) return false;
            {
                auto _m25 = save();
                bool _ok25 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m25);
                if (_ok25) return false;
            }
            out = opgen::ValueType::TyUleb32;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("sleb128")) return false;
            {
                auto _m26 = save();
                bool _ok26 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m26);
                if (_ok26) return false;
            }
            out = opgen::ValueType::TySleb32;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("sleb64")) return false;
            {
                auto _m27 = save();
                bool _ok27 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m27);
                if (_ok27) return false;
            }
            out = opgen::ValueType::TySleb64;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("sleb32")) return false;
            {
                auto _m28 = save();
                bool _ok28 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m28);
                if (_ok28) return false;
            }
            out = opgen::ValueType::TySleb32;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("memarg")) return false;
            {
                auto _m29 = save();
                bool _ok29 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m29);
                if (_ok29) return false;
            }
            out = opgen::ValueType::TyMemarg;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("blocktype")) return false;
            {
                auto _m30 = save();
                bool _ok30 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m30);
                if (_ok30) return false;
            }
            out = opgen::ValueType::TyBlockType;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("brtable")) return false;
            {
                auto _m31 = save();
                bool _ok31 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m31);
                if (_ok31) return false;
            }
            out = opgen::ValueType::TyBrTable;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("trytable")) return false;
            {
                auto _m32 = save();
                bool _ok32 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m32);
                if (_ok32) return false;
            }
            out = opgen::ValueType::TyTryTable;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("selectvec")) return false;
            {
                auto _m33 = save();
                bool _ok33 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m33);
                if (_ok33) return false;
            }
            out = opgen::ValueType::TySelectVec;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("any")) return false;
        {
            auto _m34 = save();
            bool _ok34 = [&]() -> bool {
                if (at_end() || !is_ident_continue(peek())) return false;
                advance();
                return true;
            }();
            restore(_m34);
            if (_ok34) return false;
        }
        out = opgen::ValueType::TyAny;
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

bool Parser::parse_LocalValueKind(opgen::LocalValueKind& out) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("short")) return false;
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
            out = opgen::LocalValueKind::LvShort;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("int")) return false;
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
            out = opgen::LocalValueKind::LvInt;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("ref")) return false;
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
            out = opgen::LocalValueKind::LvRef;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("long")) return false;
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
            out = opgen::LocalValueKind::LvLong;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("float")) return false;
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
            out = opgen::LocalValueKind::LvFloat;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("double")) return false;
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
        out = opgen::LocalValueKind::LvDouble;
        }
        }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_Annotation(std::vector<opgen::ErrorCase*>& errs, std::vector<const char*>& flags, std::optional<opgen::LocalValueDecl*>& local_value, std::vector<opgen::VerifyRejectCase*>& vrs, std::vector<opgen::EdgeCase*>& edges, std::optional<opgen::Branch*>& br, std::vector<opgen::AnnotationFact*>& facts, std::optional<opgen::LocalIndexDecl*>& local_index, std::optional<opgen::SwitchPayloadDecl*>& switch_payload, std::optional<opgen::NarrowFormDecl*>& narrow_form, std::optional<opgen::SpecRef*>& spec, std::optional<const char*>& setup, std::optional<const char*>& verify) {
    peg::Span fname; peg::Span vr_name; peg::Span s1; peg::Span s2;
       peg::Span maj_s; peg::Span min_s; peg::Span pat_s;
       peg::Span li_s; peg::Span sp_s; peg::Span fact_name; peg::Span fact_val;
       opgen::LocalValueKind lvk;
       opgen::SemExpr* cond = nullptr;
       std::string blk;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("error:")) return false;
            {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_SemCond(cond)) return false;
                    skip();
                    if (!match("->")) return false;
                    return true;
                }()) restore(_m1);
            }
            skip();
            if (!ident(fname)) return false;
            errs.push_back(new opgen::ErrorCase(opgen_opt(cond), opgen_dup(fname)));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("flag:")) return false;
            skip();
            if (!ident(fname)) return false;
            flags.push_back(opgen_dup(fname));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("local_value:")) return false;
            skip();
            if (!parse_LocalValueKind(lvk)) return false;
            local_value = new opgen::LocalValueDecl(lvk);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("verify_reject:")) return false;
            skip();
            if (!ident(vr_name)) return false;
            skip();
            if (!string_lit(s1)) return false;
            skip();
            if (!match("->")) return false;
            skip();
            if (!ident(fname)) return false;
            vrs.push_back(new opgen::VerifyRejectCase(
               opgen_dup(vr_name), opgen_dup(s1), opgen_dup(fname)));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("edge:")) return false;
            skip();
            if (!string_lit(s1)) return false;
            skip();
            if (!match("->")) return false;
            skip();
            if (!string_lit(s2)) return false;
            edges.push_back(new opgen::EdgeCase(opgen_dup(s1), opgen_dup(s2)));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("branch:")) return false;
            skip();
            if (!string_lit(s1)) return false;
            skip();
            if (!match("->")) return false;
            skip();
            if (!string_lit(s2)) return false;
            br = new opgen::Branch(opgen_dup(s1), opgen_dup(s2));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("setup:")) return false;
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", blk)) return false;
           setup = opgen_trim_intern(blk);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("verify:")) return false;
            skip();
            if (!match("(.")) return false;
            if (!scan_to("." ")", blk)) return false;
           verify = opgen_trim_intern(blk);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("spec:")) return false;
            skip();
            if (!integer(maj_s)) return false;
            skip();
            if (!match(".")) return false;
            skip();
            if (!integer(min_s)) return false;
            skip();
            if (!match(".")) return false;
            skip();
            if (!integer(pat_s)) return false;
            spec = new opgen::SpecRef((int32_t)opgen_parse_int(maj_s),
                                     (int32_t)opgen_parse_int(min_s),
                                     (int32_t)opgen_parse_int(pat_s));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("local_index:")) return false;
            skip();
            if (!integer(li_s)) return false;
            local_index = new opgen::LocalIndexDecl((int32_t)opgen_parse_int(li_s));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("switch_payload:")) return false;
            skip();
            if (!integer(sp_s)) return false;
            switch_payload = new opgen::SwitchPayloadDecl((int32_t)opgen_parse_int(sp_s));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("narrow_form:")) return false;
            skip();
            if (!ident(fname)) return false;
            narrow_form = new opgen::NarrowFormDecl(opgen_dup(fname));
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!ident(fact_name)) return false;
        skip();
        if (!match(":")) return false;
        skip();
        if (!ident(fact_val)) return false;
        facts.push_back(new opgen::AnnotationFact(opgen_dup(fact_name),
                                                     opgen_dup(fact_val)));
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
    }
    return true;
}

bool Parser::parse_FactsDecl(std::vector<opgen::FactDecl*>& out) {
    peg::Span ns; peg::Span ms; std::vector<const char*> members;
    skip();
    if (!match("facts")) return false;
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
    if (!ident(ns)) return false;
    skip();
    if (!match("=")) return false;
    skip();
    if (!ident(ms)) return false;
    members.push_back(opgen_dup(ms));
    for (;;) {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match(",")) return false;
            skip();
            if (!ident(ms)) return false;
            members.push_back(opgen_dup(ms));
            return true;
        }()) { restore(_m1); break; }
    }
    out.push_back(new opgen::FactDecl(opgen_dup(ns), std::move(members)));
    return true;
}

bool Parser::parse_SemBody(std::vector<opgen::SemStmt*>& body) {
    skip();
    if (!match("(")) return false;
    skip();
    if (!match(".")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_SemBlockStmt(body)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    {
        auto _m1 = save();
        if ([&]() -> bool {
            skip();
            if (!match(".")) return false;
            skip();
            if (!match(")")) return false;
            return true;
        }()) {} else {
        restore(_m1);
        return false;
        }
    }
    return true;
}

bool Parser::parse_SemCond(opgen::SemExpr*& out) {
    skip();
    if (!match("(")) return false;
    skip();
    if (!match(".")) return false;
    skip();
    if (!parse_SemExpr(out)) return false;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match(".")) return false;
            skip();
            if (!match(")")) return false;
            return true;
        }()) {} else {
        restore(_m0);
        return false;
        }
    }
    return true;
}

bool Parser::parse_SemBlockStmt(std::vector<opgen::SemStmt*>& list) {
    opgen::SemStmt* st = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_SemLocalDecl(list)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_SemStmt(st)) return false;
        if (st) list.push_back(st);
        }
    }
    return true;
}

bool Parser::parse_SemLocalDecl(std::vector<opgen::SemStmt*>& list) {
    const char* ty = nullptr;
    skip();
    if (!parse_JavaPrimType(ty)) return false;
    skip();
    if (!parse_SemDeclarator(list, ty)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match(",")) return false;
            skip();
            if (!parse_SemDeclarator(list, ty)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match(";")) return false;
    return true;
}

bool Parser::parse_SemDeclarator(std::vector<opgen::SemStmt*>& list, const char* ty) {
    peg::Span ns; opgen::SemExpr* init = nullptr;
    skip();
    if (!ident(ns)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("=")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    skip();
                    if (!match("=")) return false;
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!parse_SemExpr(init)) return false;
            return true;
        }()) restore(_m0);
    }
    list.push_back(new opgen::SLocalDecl(ty, opgen_dup(ns), opgen_opt(init)));
    return true;
}

bool Parser::parse_SemStmt(opgen::SemStmt*& out) {
    opgen::SemExpr* cond = nullptr; opgen::SemStmt* then_ = nullptr;
       opgen::SemStmt* els = nullptr;
       opgen::SemStmt* init = nullptr; opgen::SemStmt* upd = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_SemBlock(out)) return false;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("if")) return false;
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
            if (!match("(")) return false;
            skip();
            if (!parse_SemExpr(cond)) return false;
            skip();
            if (!match(")")) return false;
            skip();
            if (!parse_SemStmt(then_)) return false;
            {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("else")) return false;
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
                    if (!parse_SemStmt(els)) return false;
                    return true;
                }()) restore(_m2);
            }
            out = new opgen::SIf(cond, then_, opgen_opt(els));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("while")) return false;
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
            if (!parse_SemExpr(cond)) return false;
            skip();
            if (!match(")")) return false;
            skip();
            if (!parse_SemStmt(then_)) return false;
            out = new opgen::SWhile(cond, then_);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("for")) return false;
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
            if (!match("(")) return false;
            skip();
            if (!parse_ForClause(init)) return false;
            skip();
            if (!match(";")) return false;
            {
                auto _m6 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_SemExpr(cond)) return false;
                    return true;
                }()) restore(_m6);
            }
            skip();
            if (!match(";")) return false;
            skip();
            if (!parse_ForClause(upd)) return false;
            skip();
            if (!match(")")) return false;
            skip();
            if (!parse_SemStmt(then_)) return false;
            out = new opgen::SFor(opgen_opt(init), opgen_opt(cond), opgen_opt(upd), then_);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match(";")) return false;
            out = nullptr;
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("trap")) return false;
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
            if (!match(";")) return false;
            out = new opgen::STrap();
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_SemSimpleStmt(out)) return false;
        }
        }
        }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_ForClause(opgen::SemStmt*& out) {
    const char* ty = nullptr; peg::Span ns; opgen::SemExpr* init = nullptr;
       opgen::SemExpr* lhs = nullptr; opgen::SemExpr* rhs = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!parse_JavaPrimType(ty)) return false;
            skip();
            if (!ident(ns)) return false;
            {
                auto _m1 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match("=")) return false;
                    {
                        auto _m2 = save();
                        bool _ok2 = [&]() -> bool {
                            skip();
                            if (!match("=")) return false;
                            return true;
                        }();
                        restore(_m2);
                        if (_ok2) return false;
                    }
                    skip();
                    if (!parse_SemExpr(init)) return false;
                    return true;
                }()) restore(_m1);
            }
            out = new opgen::SLocalDecl(ty, opgen_dup(ns), opgen_opt(init));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!parse_SemExpr(lhs)) return false;
            skip();
            if (!match("=")) return false;
            {
                auto _m3 = save();
                bool _ok3 = [&]() -> bool {
                    skip();
                    if (!match("=")) return false;
                    return true;
                }();
                restore(_m3);
                if (_ok3) return false;
            }
            skip();
            if (!parse_SemExpr(rhs)) return false;
            out = new opgen::SAssign(lhs, rhs);
            return true;
        }()) {} else {
        restore(_m0);
        out = nullptr;
        }
        }
    }
    return true;
}

bool Parser::parse_SemBlock(opgen::SemStmt*& out) {
    std::vector<opgen::SemStmt*> blk;
    skip();
    if (!match("{")) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_SemBlockStmt(blk)) return false;
            return true;
        }()) { restore(_m0); break; }
    }
    skip();
    if (!match("}")) return false;
    out = new opgen::SBlock(std::move(blk));
    return true;
}

bool Parser::parse_SemSimpleStmt(opgen::SemStmt*& out) {
    opgen::SemExpr* lhs = nullptr; opgen::SemExpr* rhs = nullptr;
    skip();
    if (!parse_SemExpr(lhs)) return false;
    skip();
    if (peek_at("=")) {
        skip();
        if (!match("=")) return false;
        {
            auto _m0 = save();
            bool _ok0 = [&]() -> bool {
                skip();
                if (!match("=")) return false;
                return true;
            }();
            restore(_m0);
            if (_ok0) return false;
        }
        skip();
        if (!parse_SemExpr(rhs)) return false;
        skip();
        if (!match(";")) return false;
        out = new opgen::SAssign(lhs, rhs);
    } else {
        skip();
        if (!match(";")) return false;
        out = new opgen::SExprStmt(lhs);
    }
    return true;
}

bool Parser::parse_SemExpr(opgen::SemExpr*& out) {
    skip();
    if (!parse_SemTernary(out)) return false;
    return true;
}

bool Parser::parse_SemTernary(opgen::SemExpr*& out) {
    opgen::SemExpr* c = nullptr; opgen::SemExpr* t = nullptr; opgen::SemExpr* e = nullptr;
    skip();
    if (!parse_SemCondOr(c)) return false;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("?")) return false;
            skip();
            if (!parse_SemExpr(t)) return false;
            skip();
            if (!match(":")) return false;
            skip();
            if (!parse_SemTernary(e)) return false;
            c = new opgen::STernary(c, t, e);
            return true;
        }()) restore(_m0);
    }
    out = c;
    return true;
}

bool Parser::parse_SemCondOr(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr;
    skip();
    if (!parse_SemCondAnd(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("||")) return false;
            skip();
            if (!parse_SemCondAnd(rhs)) return false;
            acc = new opgen::SBinOp("||", acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemCondAnd(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr;
    skip();
    if (!parse_SemBinOr(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("&&")) return false;
            skip();
            if (!parse_SemBinOr(rhs)) return false;
            acc = new opgen::SBinOp("&&", acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemBinOr(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr;
    skip();
    if (!parse_SemBinXor(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("|")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    skip();
                    if (!match("|")) return false;
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!parse_SemBinXor(rhs)) return false;
            acc = new opgen::SBinOp("|", acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemBinXor(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr;
    skip();
    if (!parse_SemBinAnd(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("^")) return false;
            skip();
            if (!parse_SemBinAnd(rhs)) return false;
            acc = new opgen::SBinOp("^", acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemBinAnd(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr;
    skip();
    if (!parse_SemEq(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("&")) return false;
            {
                auto _m1 = save();
                bool _ok1 = [&]() -> bool {
                    skip();
                    if (!match("&")) return false;
                    return true;
                }();
                restore(_m1);
                if (_ok1) return false;
            }
            skip();
            if (!parse_SemEq(rhs)) return false;
            acc = new opgen::SBinOp("&", acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemEq(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr; const char* op = nullptr;
    skip();
    if (!parse_SemRel(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("==")) {
                skip();
                if (!match("==")) return false;
                op = "==";
            } else {
                skip();
                if (!match("!=")) return false;
                op = "!=";
            }
            skip();
            if (!parse_SemRel(rhs)) return false;
            acc = new opgen::SBinOp(op, acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemRel(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr; const char* op = nullptr;
    skip();
    if (!parse_SemShift(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!match("<=")) return false;
                    op = "<=";
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match(">=")) return false;
                    op = ">=";
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match("<")) return false;
                    {
                        auto _m2 = save();
                        bool _ok2 = [&]() -> bool {
                            skip();
                            if (!match("<")) return false;
                            return true;
                        }();
                        restore(_m2);
                        if (_ok2) return false;
                    }
                    op = "<";
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!match(">")) return false;
                {
                    auto _m3 = save();
                    bool _ok3 = [&]() -> bool {
                        skip();
                        if (!match(">")) return false;
                        return true;
                    }();
                    restore(_m3);
                    if (_ok3) return false;
                }
                op = ">";
                }
                }
                }
            }
            skip();
            if (!parse_SemShift(rhs)) return false;
            acc = new opgen::SBinOp(op, acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemShift(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr; const char* op = nullptr;
    skip();
    if (!parse_SemAdd(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            {
                auto _m1 = save();
                if ([&]() -> bool {
                    skip();
                    if (!match(">>>")) return false;
                    op = ">>>";
                    return true;
                }()) {} else {
                restore(_m1);
                if ([&]() -> bool {
                    skip();
                    if (!match(">>")) return false;
                    op = ">>";
                    return true;
                }()) {} else {
                restore(_m1);
                skip();
                if (!match("<<")) return false;
                op = "<<";
                }
                }
            }
            skip();
            if (!parse_SemAdd(rhs)) return false;
            acc = new opgen::SBinOp(op, acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemAdd(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr; const char* op = nullptr;
    skip();
    if (!parse_SemMul(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("+")) {
                skip();
                if (!match("+")) return false;
                op = "+";
            } else {
                skip();
                if (!match("-")) return false;
                op = "-";
            }
            skip();
            if (!parse_SemMul(rhs)) return false;
            acc = new opgen::SBinOp(op, acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemMul(opgen::SemExpr*& out) {
    opgen::SemExpr* acc = nullptr; opgen::SemExpr* rhs = nullptr; const char* op = nullptr;
    skip();
    if (!parse_SemUnary(acc)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (peek_at("*")) {
                skip();
                if (!match("*")) return false;
                op = "*";
            } else             if (peek_at("/")) {
                skip();
                if (!match("/")) return false;
                op = "/";
            } else {
                skip();
                if (!match("%")) return false;
                op = "%";
            }
            skip();
            if (!parse_SemUnary(rhs)) return false;
            acc = new opgen::SBinOp(op, acc, rhs);
            return true;
        }()) { restore(_m0); break; }
    }
    out = acc;
    return true;
}

bool Parser::parse_SemUnary(opgen::SemExpr*& out) {
    opgen::SemExpr* e = nullptr; const char* op = nullptr; const char* ct = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (peek_at("-")) {
                skip();
                if (!match("-")) return false;
                op = "-";
            } else             if (peek_at("~")) {
                skip();
                if (!match("~")) return false;
                op = "~";
            } else {
                skip();
                if (!match("!")) return false;
                {
                    auto _m1 = save();
                    bool _ok1 = [&]() -> bool {
                        skip();
                        if (!match("=")) return false;
                        return true;
                    }();
                    restore(_m1);
                    if (_ok1) return false;
                }
                op = "!";
            }
            skip();
            if (!parse_SemUnary(e)) return false;
            out = new opgen::SUnary(op, e);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("(")) return false;
            skip();
            if (!parse_JavaPrimType(ct)) return false;
            skip();
            if (!match(")")) return false;
            skip();
            if (!parse_SemUnary(e)) return false;
            out = new opgen::SCast(ct, e);
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_SemPostfix(out)) return false;
        }
        }
    }
    return true;
}

bool Parser::parse_SemPostfix(opgen::SemExpr*& out) {
    opgen::SemExpr* e = nullptr; opgen::SemExpr* idx = nullptr;
    skip();
    if (!parse_SemPrimary(e)) return false;
    for (;;) {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("[")) return false;
            skip();
            if (!parse_SemExpr(idx)) return false;
            skip();
            if (!match("]")) return false;
            e = new opgen::SIndex(e, idx);
            return true;
        }()) { restore(_m0); break; }
    }
    out = e;
    return true;
}

bool Parser::parse_SemPrimary(opgen::SemExpr*& out) {
    peg::Span is; peg::Span fs; peg::Span ns; opgen::SemExpr* inner = nullptr;
       std::vector<opgen::SemExpr*> args; opgen::SemExpr* arg = nullptr;
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!floating(fs)) return false;
            out = new opgen::SFloat(opgen_parse_float(fs));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!integer(is)) return false;
            out = new opgen::SInt(opgen_parse_int(is));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("null")) return false;
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
            out = new opgen::SInt(0);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("true")) return false;
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
            out = new opgen::SInt(1);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("false")) return false;
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
            out = new opgen::SInt(0);
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!ident(ns)) return false;
            skip();
            if (!match("(")) return false;
            {
                auto _m4 = save();
                if (![&]() -> bool {
                    skip();
                    if (!parse_SemExpr(arg)) return false;
                    args.push_back(arg);
                    for (;;) {
                        auto _m5 = save();
                        if (![&]() -> bool {
                            skip();
                            if (!match(",")) return false;
                            skip();
                            if (!parse_SemExpr(arg)) return false;
                            args.push_back(arg);
                            return true;
                        }()) { restore(_m5); break; }
                    }
                    return true;
                }()) restore(_m4);
            }
            skip();
            if (!match(")")) return false;
            out = new opgen::SCall(opgen_dup(ns), std::move(args));
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!ident(ns)) return false;
            out = new opgen::SIdent(opgen_dup(ns));
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("(")) return false;
        skip();
        if (!parse_SemExpr(inner)) return false;
        skip();
        if (!match(")")) return false;
        out = inner;
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

bool Parser::parse_MethodType(const char*& out) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("void")) return false;
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
            out = "void";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("status")) return false;
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
            out = "status";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("ref")) return false;
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
            out = "ref";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("any")) return false;
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
            out = "any";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("v128")) return false;
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
            out = "v128";
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!parse_JavaPrimType(out)) return false;
        }
        }
        }
        }
        }
    }
    return true;
}

bool Parser::parse_MethodParam(std::vector<opgen::MethodParam*>& list) {
    const char* ty = nullptr; peg::Span ns;
    skip();
    if (!parse_MethodType(ty)) return false;
    skip();
    if (!ident(ns)) return false;
    list.push_back(new opgen::MethodParam(ty, opgen_dup(ns)));
    return true;
}

bool Parser::parse_StatusDecl(std::optional<opgen::StatusDecl*>& out) {
    peg::Span ty; peg::Span ok; peg::Span er; peg::Span ha; peg::Span ex;
       std::vector<const char*> extra;
    skip();
    if (!match("status")) return false;
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
    if (!ident(ty)) return false;
    skip();
    if (!ident(ok)) return false;
    skip();
    if (!ident(er)) return false;
    skip();
    if (!ident(ha)) return false;
    {
        auto _m1 = save();
        if (![&]() -> bool {
            skip();
            if (!match("[")) return false;
            skip();
            if (!ident(ex)) return false;
            extra.push_back(opgen_dup(ex));
            for (;;) {
                auto _m2 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!ident(ex)) return false;
                    extra.push_back(opgen_dup(ex));
                    return true;
                }()) { restore(_m2); break; }
            }
            skip();
            if (!match("]")) return false;
            return true;
        }()) restore(_m1);
    }
    out = new opgen::StatusDecl(opgen_dup(ty), opgen_dup(ok), opgen_dup(er),
                                     opgen_dup(ha), std::move(extra));
    return true;
}

bool Parser::parse_BackendDecl(std::optional<const char*>& out) {
    peg::Span hdr;
    skip();
    if (!match("backend")) return false;
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
    if (!string_lit(hdr)) return false;
    out = opgen_dup(hdr);
    return true;
}

bool Parser::parse_SidetableDecl(bool& out) {
    skip();
    if (!match("sidetable")) return false;
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
    out = true;
    return true;
}

bool Parser::parse_TypeDecl(std::vector<opgen::TypeDecl*>& list) {
    opgen::ValueType ty; peg::Span sc; peg::Span usc; peg::Span fld; peg::Span sl;
    skip();
    if (!match("type")) return false;
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
    if (!parse_ValueType(ty)) return false;
    skip();
    if (!ident(sc)) return false;
    skip();
    if (!ident(usc)) return false;
    skip();
    if (!integer(sl)) return false;
    skip();
    if (!ident(fld)) return false;
    list.push_back(new opgen::TypeDecl(ty, opgen_dup(sc), opgen_dup(usc),
             (int32_t)opgen_parse_int(sl), opgen_dup(fld)));
    return true;
}

bool Parser::parse_MethodDecl(std::vector<opgen::MethodDecl*>& list) {
    bool native = false; bool inl = false; const char* rty = nullptr; peg::Span ns;
       std::vector<opgen::MethodParam*> params;
       std::vector<opgen::SemStmt*> body;
    {
        auto _m0 = save();
        if (![&]() -> bool {
            skip();
            if (!match("inline")) return false;
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
            inl = true;
            return true;
        }()) restore(_m0);
    }
    {
        auto _m2 = save();
        if (![&]() -> bool {
            skip();
            if (!match("native")) return false;
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
            native = true;
            return true;
        }()) restore(_m2);
    }
    skip();
    if (!parse_MethodType(rty)) return false;
    skip();
    if (!ident(ns)) return false;
    skip();
    if (!match("(")) return false;
    {
        auto _m4 = save();
        if (![&]() -> bool {
            skip();
            if (!parse_MethodParam(params)) return false;
            for (;;) {
                auto _m5 = save();
                if (![&]() -> bool {
                    skip();
                    if (!match(",")) return false;
                    skip();
                    if (!parse_MethodParam(params)) return false;
                    return true;
                }()) { restore(_m5); break; }
            }
            return true;
        }()) restore(_m4);
    }
    skip();
    if (!match(")")) return false;
    skip();
    if (peek_at(";")) {
        skip();
        if (!match(";")) return false;
    } else {
        skip();
        if (!match("{")) return false;
        for (;;) {
            auto _m6 = save();
            if (![&]() -> bool {
                skip();
                if (!parse_SemBlockStmt(body)) return false;
                return true;
            }()) { restore(_m6); break; }
        }
        skip();
        if (!match("}")) return false;
    }
    list.push_back(new opgen::MethodDecl(native, inl, rty, opgen_dup(ns),
             std::move(params), std::move(body)));
    return true;
}

bool Parser::parse_JavaPrimType(const char*& out) {
    {
        auto _m0 = save();
        if ([&]() -> bool {
            skip();
            if (!match("boolean")) return false;
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
            out = "boolean";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("byte")) return false;
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
            out = "byte";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("short")) return false;
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
            out = "short";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("char")) return false;
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
            out = "char";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("int")) return false;
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
            out = "int";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("long")) return false;
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
            out = "long";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("uint")) return false;
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
            out = "uint";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("ulong")) return false;
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
            out = "ulong";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("float")) return false;
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
            out = "float";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("double")) return false;
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
            out = "double";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("unsigned")) return false;
            {
                auto _m11 = save();
                bool _ok11 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m11);
                if (_ok11) return false;
            }
            out = "unsigned";
            return true;
        }()) {} else {
        restore(_m0);
        if ([&]() -> bool {
            skip();
            if (!match("signed")) return false;
            {
                auto _m12 = save();
                bool _ok12 = [&]() -> bool {
                    if (at_end() || !is_ident_continue(peek())) return false;
                    advance();
                    return true;
                }();
                restore(_m12);
                if (_ok12) return false;
            }
            out = "signed";
            return true;
        }()) {} else {
        restore(_m0);
        skip();
        if (!match("any")) return false;
        {
            auto _m13 = save();
            bool _ok13 = [&]() -> bool {
                if (at_end() || !is_ident_continue(peek())) return false;
                advance();
                return true;
            }();
            restore(_m13);
            if (_ok13) return false;
        }
        out = "any";
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
    }
    return true;
}


