/*
 * coverage_node.h — node fixture shared by the burgc rewrite-error e2e tests.
 *
 * Deliberately plain C so ONE fixture drives both the C and the C++ backend: the two
 * e2e drivers compile the same grammar against the same nodes and assert the same
 * behaviour, differing only in how a failure surfaces (error sink vs. thrown BurgError).
 * That is the parity claim these tests exist to hold up.
 */
#ifndef BURGC_COVERAGE_NODE_H
#define BURGC_COVERAGE_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Terminal opcodes. Orphan is the point of the fixture: it is a declared terminal that
 * no derivation chain connects to the start nonterminal, so a tree rooted at one is
 * legally parseable and illegally uncoverable. */
enum {
    COV_CONST  = 1,
    COV_ADD    = 2,
    COV_ORPHAN = 3,
    /* Variable arity: Call's rule declares NO children, yet a Call node may carry
     * them. The matcher reduces such extras with the start nonterminal — and per
     * iburg (p.4: "Each C sums the costs of the non-terminals on the right-hand
     * side"; p.7's state() is that sentence as code) it must COST them the same
     * way, or the root's cost[start] stops being the cover's cost and the DP's
     * minimality claim is over covers of the wrong tree. */
    COV_CALL   = 4
};

typedef struct CovNode {
    int             op;
    int             id;         /* small, explicit — see BURG_NODE_ID below */
    int             nkids;
    struct CovNode* kids[2];
    int             nsucc;
    struct CovNode* succs[2];
} CovNode;

/* Rule actions append their tag here, so a driver can assert WHICH rules fired and in
 * what order — not merely that a rewrite returned. */
#define COV_TRACE_MAX 64
extern int cov_trace[COV_TRACE_MAX];
extern int cov_trace_len;

void cov_record(int tag);
void cov_trace_reset(void);

#ifdef __cplusplus
}
#endif

/* The consumer contract burgc's generated matcher compiles against. */
#define BURG_NODE_TYPE          CovNode*
#define BURG_NODE_OP(n)         ((n)->op)
#define BURG_NODE_ARITY(n)      ((n)->nkids)
#define BURG_NODE_CHILD(n, i)   ((n)->kids[i])
/* An explicit small id rather than the pointer: the C backend narrows the id to uint32_t
 * for its htree key, and a fixture should not depend on where malloc put things. */
#define BURG_NODE_ID(n)         ((void*)(uintptr_t)((n)->id))
#define BURG_NODE_SUCC_COUNT(n) ((n)->nsucc)
#define BURG_NODE_SUCC(n, i)    ((n)->succs[i])

#endif /* BURGC_COVERAGE_NODE_H */
