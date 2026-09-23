/* halt_main.c — drives halt.peg's generated C parser. */
#include "halt_parser.h"

#include <stdio.h>
#include <string.h>

static int run(const char* src, int halt_at, halt_ctx_t* hc, bool* ok) {
    peg_state p;
    memset(hc, 0, sizeof *hc);
    hc->halt_at = halt_at;
    halt_parser_init(&p, src, (int)strlen(src));
    p.user_data = hc;
    *ok = halt_parser_parse(&p);
    return p.halted;
}

static int expect(const char* what, const halt_ctx_t* hc, bool ok,
                  bool want_ok, const int* want, int nwant) {
    int bad = ok != want_ok || hc->nran != nwant;
    for (int i = 0; !bad && i < nwant; i++) bad = hc->ran[i] != want[i];
    if (bad) {
        fprintf(stderr, "FAIL %s: parse %s, actions", what, ok ? "ok" : "failed");
        for (int i = 0; i < hc->nran; i++) fprintf(stderr, " %d", hc->ran[i]);
        fprintf(stderr, "\n");
    }
    return bad;
}

int main(void) {
    halt_ctx_t hc;
    bool ok;
    int rc = 0;

    /* Unhalted: every action, in order, and the parse succeeds. */
    static const int all[] = { 1, 2, 1, 99 };
    int halted = run("x y x", 0, &hc, &ok);
    rc |= expect("unhalted", &hc, ok, true, all, 4) | halted;

    /* Halted by the `y` action: the `x` after it does not match, the
     * closing action does not run, and the parse fails. */
    static const int to_y[] = { 1, 2 };
    halted = run("x y x", 2, &hc, &ok);
    rc |= expect("halted mid-repetition", &hc, ok, false, to_y, 2) | !halted;

    /* Halted by the last item's action: nothing is left to match, and
     * the action after the repetition still does not run. */
    static const int to_end[] = { 1, 1, 2 };
    halted = run("x x y", 2, &hc, &ok);
    rc |= expect("halted at the last item", &hc, ok, false, to_end, 3) | !halted;

    if (!rc) printf("pegc halt: ok\n");
    return rc;
}
