#include "coverage_node.h"

int cov_trace[COV_TRACE_MAX];
int cov_trace_len = 0;

void cov_record(int tag) {
    if (cov_trace_len < COV_TRACE_MAX)
        cov_trace[cov_trace_len++] = tag;
}

void cov_trace_reset(void) {
    cov_trace_len = 0;
}
