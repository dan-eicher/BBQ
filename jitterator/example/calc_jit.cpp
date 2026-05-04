// CLI debugger over the calc JIT pipeline. Two modes:
//   calc_jit "<expression>"          → parse + JIT-compile + run
//   calc_jit --dump "<expression>"   → parse + DDCG-compile, print
//                                       the IR continuation graph, exit
//                                       (no JIT, no execution)
//
// `--dump` walks the post-DDCG IR and prints each node, distinguishing
// operand sub-trees (BURG `reg`/`stack_reg` — paper § value sub-trees)
// from spine successors (`next`/`then_`/`else_`/`target`). Useful for
// verifying what calc.ddcg + the normalizer produced before BURG
// lowers it to stencils.

#include "calc_runner.h"
#include "compiler.h"
#include "frontend/CalcIR.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace {

const char* tag_name(calc_ir::NodeTag t) {
    using T = calc_ir::NodeTag;
    switch (t) {
    case T::LoadConst:  return "LoadConst";
    case T::LoadLocal:  return "LoadLocal";
    case T::Add:        return "Add";
    case T::Sub:        return "Sub";
    case T::Mul:        return "Mul";
    case T::Neg:        return "Neg";
    case T::CmpEq:      return "CmpEq";
    case T::CmpNeq:     return "CmpNeq";
    case T::CmpLt:      return "CmpLt";
    case T::CmpLe:      return "CmpLe";
    case T::CmpGt:      return "CmpGt";
    case T::CmpGe:      return "CmpGe";
    case T::StoreLocal: return "StoreLocal";
    case T::ExprEffect: return "ExprEffect";
    case T::Call:       return "Call";
    case T::Branch:     return "Branch";
    case T::Goto:       return "Goto";
    case T::Nop:        return "Nop";
    case T::Halt:       return "Halt";
    case T::Leave:      return "Leave";
    }
    return "?";
}

void print_fields(calc_ir::Node* n) {
    using T = calc_ir::NodeTag;
    switch (n->tag) {
    case T::LoadConst:
        std::printf(" value=%lld",
                    (long long)static_cast<calc_ir::LoadConst*>(n)->value);
        break;
    case T::LoadLocal:
        std::printf(" src=%lld",
                    (long long)static_cast<calc_ir::LoadLocal*>(n)->src);
        break;
    case T::StoreLocal:
        std::printf(" dest=%lld",
                    (long long)static_cast<calc_ir::StoreLocal*>(n)->dest);
        break;
    case T::Call: {
        auto* c = static_cast<calc_ir::Call*>(n);
        std::printf(" n_args=%zu frame_size=%lld",
                    c->args.size(), (long long)c->frame_size);
        break;
    }
    default: break;
    }
}

void indent(int depth) {
    for (int i = 0; i < depth; i++) std::printf("  ");
}

void dump(calc_ir::Node* n, std::unordered_set<calc_ir::Node*>& seen, int depth) {
    if (!n) {
        indent(depth);
        std::printf("(null)\n");
        return;
    }
    indent(depth);
    std::printf("[%p] %s", (void*)n, tag_name(n->tag));
    print_fields(n);
    if (!seen.insert(n).second) {
        std::printf(" [seen]\n");
        return;
    }
    std::printf("\n");

    int oc = operand_count(n);
    for (int i = 0; i < oc; i++) {
        indent(depth + 1);
        std::printf("op[%d]:\n", i);
        dump(operand_producer(n, i), seen, depth + 2);
    }
    int sc = successor_count(n);
    for (int i = 0; i < sc; i++) {
        indent(depth + 1);
        std::printf("succ[%d]:\n", i);
        dump(successor(n, i), seen, depth + 2);
    }
}

int do_dump(const char* input) {
    calc_ir::Node* ir = calc_runner::calc_compile(input);
    if (!ir) {
        std::fprintf(stderr, "Parse error\n");
        return 1;
    }
    std::unordered_set<calc_ir::Node*> seen;
    dump(ir, seen, 0);
    return 0;
}

int do_run(const char* input) {
    bool ok = false;
    int64_t result = calc_runner::calc_run(input, &ok);
    if (!ok) {
        std::fprintf(stderr, "Parse error\n");
        return 1;
    }
    std::printf("%s = %ld\n", input, (long)result);
    return 0;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--dump] \"<expression>\"\n"
        "  --dump    parse + DDCG-compile, print IR graph, do not JIT/run\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    bool dump_mode = false;
    int argi = 1;
    if (argi < argc && std::strcmp(argv[argi], "--dump") == 0) {
        dump_mode = true;
        argi++;
    }
    if (argi >= argc) {
        usage(argv[0]);
        return 1;
    }
    const char* input = argv[argi];
    return dump_mode ? do_dump(input) : do_run(input);
}
