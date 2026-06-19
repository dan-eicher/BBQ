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
    error: "a > 100" -> overflow
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
    ASSERT_TRUE(op->errors[0]->condition.has_value());
    EXPECT_STREQ(*op->errors[0]->condition, "\"a > 100\"");  // raw, quotes kept
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
