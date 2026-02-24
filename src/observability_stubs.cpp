/**
 * Observability stub functions for uprobe-based tracing.
 *
 * When PIPANN_OBSERVABILITY is enabled, the probe macros in observability.h call
 * these functions. They are empty; the purpose is to provide stable, uprobe-able
 * symbols so bpftrace can attach and read arguments (node_id, page_id, etc.)
 * without relying on USDT (which is not fully present in the binary after static linking).
 *
 * Use with bpftrace, e.g.:
 *   uprobe:BINARY:pipeann_observe_expand_node { printf("expand node_id=%lu page_id=%lu\n", arg0, arg1); }
 *   uprobe:BINARY:pipeann_observe_tier_hit { @tier_hit[arg0]++; }
 *   uprobe:BINARY:pipeann_observe_tier_miss { @tier_miss[arg0]++; }
 */

#include <cstdint>

#if defined(PIPANN_OBSERVABILITY) && defined(HAVE_SYS_SDT_H)

extern "C" {

void pipeann_observe_expand_node(uint64_t node_id, uint64_t page_id) {
  (void) node_id;
  (void) page_id;
}

void pipeann_observe_tier_hit(uint64_t page_id) {
  (void) page_id;
}

void pipeann_observe_tier_miss(uint64_t page_id) {
  (void) page_id;
}

void pipeann_observe_read_page_request(uint64_t page_id, uint64_t offset) {
  (void) page_id;
  (void) offset;
}

}  // extern "C"

#endif  // PIPANN_OBSERVABILITY && HAVE_SYS_SDT_H
