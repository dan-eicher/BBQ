// opgen frontend tests — the pegc/asdl-generated C++ parser building the
// tag-dispatched opgen:: AST. Grows alongside the tool (Spec, lowering, emit).
#include "Parser.h"
#include <gtest/gtest.h>

namespace {

// Parse a spec string, returning the Module. AST nodes live in the static
// arena (in opgen::ASTNode), so they outlive the local Parser.
opgen::Module* parse(const char* src) {
    Parser p;
    p.init(src, (int)std::strlen(src));
    bool ok = p.parse();
    auto f = p.furthest();
    EXPECT_TRUE(ok) << "parse failed near line " << f.line << " col " << f.col;
    return ok ? p.ast : nullptr;
}

// A small spec exercising every top-level construct + the sem-action language.
const char* kSpec = R"OPGEN(
(. #include "rt.h" .)

status int OK ERR HALT

type i32 int32_t uint32_t 1 i
type i64 int64_t uint64_t 2 l

native void push(int v);

int helper(int a) {
    int x = a + 1;
    push(x);
}

iconst 0x10 [ i32 imm ] (i32 a -- i32 r)
    error: (. a > 100 .) -> overflow
    flag: pure
    (.
        int t = a + 1;
        r = t;
    .)

family ( T a, T b -- T r )
    flag: arith
    (. r = a + b; .)
{
    i32 iadd 0x20
    i64 ladd 0x21
}

%%
/* trailing body_c */
)OPGEN";

// The verifier-facing annotation vocabulary. These are per-opcode declarative
// facts about the target's verifier rules, which a corpus emitter inverts into
// probes; they carry no weight in either execution tier. Each is a typed decl,
// not free text, so a malformed one is a parse error rather than a probe that
// silently never fires.
const char* kAnnotatedSpec = R"OPGEN(
type i16 s2 u2 1 s
type i32 s4 u4 2 i

facts cp_tag         = classref, instance_fieldref, static_fieldref, static_methodref, virtual_methodref
facts expects_target = class, interface, static_method, instance_method, non_abstract, non_init, non_clinit, type_byte, type_short, type_int, type_ref
facts requires       = acc_int, instance_method_context
facts invoke_kind    = static, virtual, special, interface, super

getfield_a 0x83 [ u8 index ] (ref objectref -- ref result)
    spec: 7.5.20
    cp_tag: instance_fieldref
    expects_target: type_ref
    requires: instance_method_context
    (. result = get_field(objectref, index); .)

sload 0x16 [ u8 index ] ( -- i16 result)
    spec: 7.5.92
    local_value: short
    local_index: 0
    narrow_form: sload_0
    (. result = locals[index]; .)

sdiv 0x47 (i16 a, i16 b -- i16 result)
    spec: 7.5.28
    edge: "a == -32768 && b == -1" -> "result == -32768"
    (. result = a / b; .)

ifeq 0x60 [ i16 offset ] (i16 value -- )
    spec: 7.5.42
    branch: "value == 0" -> "pc - 2 + offset"
    (. if (value == 0) do_branch(offset); .)

dup_x 0x3F [ u8 mn ] ( -- )
    spec: 7.5.18
    verify_reject: split_int "m-boundary splits an int pair" -> TYPE_MISMATCH

invokevirtual 0x8B [ u8 index ] ( -- )
    spec: 7.5.57
    invoke_kind: virtual
    cp_tag: virtual_methodref

stableswitch 0x75 ( i16 index -- )
    spec: 7.5.94
    switch_payload: 8
)OPGEN";

// The vocabulary an opcode's declarative facts draw from is declared BY the
// spec, the same way the value model and the status outcomes are. opgen
// carries the names and checks membership; it does not know what a constant
// pool or an invoke discipline is, so a target with neither still parses and
// a target with something else entirely names its own.
const char* kFactVocabSpec = R"OPGEN(
type i32 s4 u4 1 i

facts refs    = funcidx, typeidx, memidx
facts expects = defined, imported

call 0x10 [ u16 idx ] ( -- )
    refs: funcidx
    expects: defined
    expects: imported

load 0x11 [ u16 m ] ( -- i32 r )
    refs: memidx
)OPGEN";

TEST(OpgenParse, FactVocabularyIsSpecDeclared) {
    auto* m = parse(kFactVocabSpec);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->fact_decls.size(), 2u);
    EXPECT_STREQ(m->fact_decls[0]->name, "refs");
    ASSERT_EQ(m->fact_decls[0]->members.size(), 3u);
    EXPECT_STREQ(m->fact_decls[0]->members[0], "funcidx");

    auto* call = m->opcodes[0];
    ASSERT_EQ(call->facts.size(), 3u);
    EXPECT_STREQ(call->facts[0]->name, "refs");
    EXPECT_STREQ(call->facts[0]->value, "funcidx");
    EXPECT_STREQ(call->facts[1]->name, "expects");
    EXPECT_STREQ(call->facts[1]->value, "defined");
    EXPECT_STREQ(call->facts[2]->value, "imported");

    auto* load = m->opcodes[1];
    ASSERT_EQ(load->facts.size(), 1u);
    EXPECT_STREQ(load->facts[0]->value, "memidx");
}

TEST(OpgenParse, VerifierAnnotationVocabulary) {
    auto* m = parse(kAnnotatedSpec);
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->opcodes.size(), 7u);

    auto* getfield = m->opcodes[0];
    ASSERT_TRUE(getfield->spec.has_value());
    EXPECT_EQ((*getfield->spec)->major, 7);
    EXPECT_EQ((*getfield->spec)->minor, 5);
    EXPECT_EQ((*getfield->spec)->patch, 20);
    ASSERT_EQ(getfield->facts.size(), 3u);
    EXPECT_STREQ(getfield->facts[0]->name, "cp_tag");
    EXPECT_STREQ(getfield->facts[0]->value, "instance_fieldref");
    EXPECT_STREQ(getfield->facts[1]->name, "expects_target");
    EXPECT_STREQ(getfield->facts[1]->value, "type_ref");
    EXPECT_STREQ(getfield->facts[2]->name, "requires");
    EXPECT_STREQ(getfield->facts[2]->value, "instance_method_context");

    auto* sload = m->opcodes[1];
    ASSERT_TRUE(sload->local_index.has_value());
    EXPECT_EQ((*sload->local_index)->index, 0);
    ASSERT_TRUE(sload->narrow_form.has_value());
    EXPECT_STREQ((*sload->narrow_form)->mnemonic, "sload_0");

    // A branch/edge condition is a parsed expression in the action language,
    // matching how `error:` already carries its guard.
    auto* sdiv = m->opcodes[2];
    ASSERT_EQ(sdiv->edges.size(), 1u);
    auto* ifeq = m->opcodes[3];
    ASSERT_TRUE(ifeq->br.has_value());

    auto* dup_x = m->opcodes[4];
    ASSERT_EQ(dup_x->verify_rejects.size(), 1u);
    EXPECT_STREQ(dup_x->verify_rejects[0]->name, "split_int");
    EXPECT_STREQ(dup_x->verify_rejects[0]->error_code, "TYPE_MISMATCH");

    auto* invoke = m->opcodes[5];
    ASSERT_EQ(invoke->facts.size(), 2u);
    EXPECT_STREQ(invoke->facts[0]->name, "invoke_kind");
    EXPECT_STREQ(invoke->facts[0]->value, "virtual");

    auto* sswitch = m->opcodes[6];
    ASSERT_TRUE(sswitch->switch_payload.has_value());
    EXPECT_EQ((*sswitch->switch_payload)->bytes, 8);
}

TEST(OpgenParse, TopLevelCounts) {
    auto* m = parse(kSpec);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->opcodes.size(), 3u);   // iconst + family{iadd,ladd}
    EXPECT_EQ(m->methods.size(), 2u);   // push (native) + helper
    EXPECT_EQ(m->types.size(), 2u);
    EXPECT_EQ(m->headers.size(), 1u);
    ASSERT_TRUE(m->status.has_value());
    EXPECT_STREQ((*m->status)->ok, "OK");
    EXPECT_STREQ((*m->status)->halt, "HALT");
    ASSERT_TRUE(m->body_c.has_value());
}

TEST(OpgenParse, OpcodeFieldsAndOperands) {
    auto* m = parse(kSpec);
    auto* op = m->opcodes[0];           // iconst
    EXPECT_STREQ(op->mnemonic, "iconst");
    EXPECT_EQ(op->opcode_val, 0x10);
    EXPECT_EQ(op->subop, -1);
    ASSERT_EQ(op->operands.size(), 1u);
    EXPECT_STREQ(op->operands[0]->name, "imm");
    EXPECT_EQ(op->operands[0]->ty, opgen::ValueType::TyI32);
    ASSERT_EQ(op->stack_in.size(), 1u);
    EXPECT_STREQ(op->stack_in[0]->name, "a");
    ASSERT_EQ(op->stack_out.size(), 1u);
    EXPECT_STREQ(op->stack_out[0]->name, "r");
    ASSERT_EQ(op->errors.size(), 1u);
    // The condition is a parsed expression in the action language, not a string:
    // `a > 100` is an SBinOp over the stack input, which is what gets emitted.
    ASSERT_TRUE(op->errors[0]->condition.has_value());
    const auto* cond = *op->errors[0]->condition;
    ASSERT_EQ(cond->tag, opgen::SemExprTag::SBinOp);
    const auto* bin = static_cast<const opgen::SBinOp*>(cond);
    EXPECT_STREQ(bin->op, ">");
    ASSERT_EQ(bin->left->tag, opgen::SemExprTag::SIdent);
    EXPECT_STREQ(static_cast<const opgen::SIdent*>(bin->left)->name, "a");
    EXPECT_STREQ(op->errors[0]->error_name, "overflow");
    ASSERT_EQ(op->flags.size(), 1u);
    EXPECT_STREQ(op->flags[0], "pure");
}

TEST(OpgenParse, FamilyExpandsWithTypeSubstitution) {
    auto* m = parse(kSpec);
    auto* iadd = m->opcodes[1];
    auto* ladd = m->opcodes[2];
    EXPECT_STREQ(iadd->mnemonic, "iadd");
    EXPECT_EQ(iadd->opcode_val, 0x20);
    EXPECT_STREQ(ladd->mnemonic, "ladd");
    EXPECT_EQ(ladd->opcode_val, 0x21);
    // `T a, T b -- T r` substitutes the member type into the var-typed slots.
    ASSERT_EQ(iadd->stack_in.size(), 2u);
    EXPECT_EQ(iadd->stack_in[0]->ty, opgen::ValueType::TyI32);
    EXPECT_EQ(ladd->stack_in[0]->ty, opgen::ValueType::TyI64);
    EXPECT_EQ(iadd->stack_out[0]->ty, opgen::ValueType::TyI32);
    EXPECT_EQ(ladd->stack_out[0]->ty, opgen::ValueType::TyI64);
}

// The novel bit: sem_expr/sem_stmt are tag-dispatched. Walk the iconst body
// via switch(tag) + as<T>, exactly as the lowerer will.
TEST(OpgenParse, SemBodyTagDispatch) {
    auto* m = parse(kSpec);
    auto* op = m->opcodes[0];           // iconst body: `int t = a + 1; r = t;`
    ASSERT_EQ(op->sem_body.size(), 2u);

    // stmt 0: SLocalDecl t = (a + 1)
    auto* decl = opgen::as<opgen::SLocalDecl>(op->sem_body[0]);
    ASSERT_NE(decl, nullptr) << "stmt 0 should be SLocalDecl";
    EXPECT_STREQ(decl->ty, "int");
    EXPECT_STREQ(decl->name, "t");
    ASSERT_TRUE(decl->init.has_value());
    auto* add = opgen::as<opgen::SBinOp>(*decl->init);
    ASSERT_NE(add, nullptr) << "init should be SBinOp";
    EXPECT_STREQ(add->op, "+");
    auto* lhs = opgen::as<opgen::SIdent>(add->left);
    auto* rhs = opgen::as<opgen::SInt>(add->right);
    ASSERT_NE(lhs, nullptr); EXPECT_STREQ(lhs->name, "a");
    ASSERT_NE(rhs, nullptr); EXPECT_EQ(rhs->value, 1);

    // stmt 1: SAssign r = t
    auto* asn = opgen::as<opgen::SAssign>(op->sem_body[1]);
    ASSERT_NE(asn, nullptr) << "stmt 1 should be SAssign";
    EXPECT_EQ(asn->target->tag, opgen::SemExprTag::SIdent);
    EXPECT_EQ(asn->value->tag, opgen::SemExprTag::SIdent);
}

TEST(OpgenParse, MethodNativeVsHelper) {
    auto* m = parse(kSpec);
    auto* push = m->methods[0];
    auto* helper = m->methods[1];
    EXPECT_TRUE(push->native);
    EXPECT_STREQ(push->name, "push");
    EXPECT_TRUE(push->body.empty());
    EXPECT_FALSE(helper->native);
    EXPECT_STREQ(helper->name, "helper");
    EXPECT_EQ(helper->body.size(), 2u);   // `int x = a + 1;` + `push(x);`
}

}  // namespace
