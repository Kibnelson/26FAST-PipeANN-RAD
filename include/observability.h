#pragma once

/**
 * Observability layer for eBPF and kernel I/O correlation.
 *
 * 1) Context tagging: set_io_context() so I/O can be attributed by TID or thread name.
 *    eBPF can join block_rq_issue/block_rq_complete (by pid/tid) with this context.
 * 2) USDT probes (when PIPANN_OBSERVABILITY is defined): query_start, query_done,
 *    expand_node, read_page_request, tier_hit, tier_miss for graph-walk and tier analysis.
 *
 * Build with -DPIPANN_OBSERVABILITY to enable USDT (requires sys/sdt.h, e.g. systemtap-sdt-dev).
 * Context and thread naming are always available when this header is included.
 */

#include <cstdint>

#ifdef __linux__
#include <pthread.h>
#endif

/* Only include sys/sdt.h when observability is enabled AND the header exists (e.g. systemtap-sdt-dev). */
#if defined(PIPANN_OBSERVABILITY) && defined(HAVE_SYS_SDT_H)
#include <sys/sdt.h>
#endif

/* Observability stubs: stable C symbols for uprobe-based tracing (see src/observability_stubs.cpp). */
#if defined(PIPANN_OBSERVABILITY) && defined(HAVE_SYS_SDT_H)
extern "C" {
void pipeann_observe_expand_node(uint64_t node_id, uint64_t page_id);
void pipeann_observe_tier_hit(uint64_t page_id);
void pipeann_observe_tier_miss(uint64_t page_id);
void pipeann_observe_read_page_request(uint64_t page_id, uint64_t offset);
}
#endif

/* USDT probe macros: provider "pipeann". Must be defined before set_io_context() which uses PIPANN_PROBE_IO_CONTEXT. */
#if defined(PIPANN_OBSERVABILITY) && defined(HAVE_SYS_SDT_H)
/* l_search first so bpftrace can read arg0 reliably (arg1 location often wrong with optimizations). */
#define PIPANN_PROBE_QUERY_START(l_search) \
  DTRACE_PROBE2(pipeann, query_start, (uint64_t)(l_search), (uint64_t)0)
#define PIPANN_PROBE_QUERY_DONE(total_us, n_ios, n_hops) \
  DTRACE_PROBE3(pipeann, query_done, (uint64_t)(total_us), (uint64_t)(n_ios), (uint64_t)(n_hops))
#define PIPANN_PROBE_EXPAND_NODE(node_id, page_id) do { \
  DTRACE_PROBE2(pipeann, expand_node, (uint64_t)(node_id), (uint64_t)(page_id)); \
  pipeann_observe_expand_node((uint64_t)(node_id), (uint64_t)(page_id)); \
} while (0)
#define PIPANN_PROBE_READ_PAGE_REQUEST(page_id, offset) do { \
  DTRACE_PROBE2(pipeann, read_page_request, (uint64_t)(page_id), (uint64_t)(offset)); \
  pipeann_observe_read_page_request((uint64_t)(page_id), (uint64_t)(offset)); \
} while (0)
#define PIPANN_PROBE_TIER_HIT(page_id) do { \
  DTRACE_PROBE1(pipeann, tier_hit, (uint64_t)(page_id)); \
  pipeann_observe_tier_hit((uint64_t)(page_id)); \
} while (0)
#define PIPANN_PROBE_TIER_MISS(page_id) do { \
  DTRACE_PROBE1(pipeann, tier_miss, (uint64_t)(page_id)); \
  pipeann_observe_tier_miss((uint64_t)(page_id)); \
} while (0)
#define PIPANN_PROBE_IO_CONTEXT(context_enum) \
  DTRACE_PROBE1(pipeann, io_context, (uint64_t)(context_enum))
#else
#define PIPANN_PROBE_QUERY_START(l_search) ((void)0)
#define PIPANN_PROBE_QUERY_DONE(total_us, n_ios, n_hops) ((void)0)
#define PIPANN_PROBE_EXPAND_NODE(node_id, page_id) ((void)0)
#define PIPANN_PROBE_READ_PAGE_REQUEST(page_id, offset) ((void)0)
#define PIPANN_PROBE_TIER_HIT(page_id) ((void)0)
#define PIPANN_PROBE_TIER_MISS(page_id) ((void)0)
#define PIPANN_PROBE_IO_CONTEXT(context_enum) ((void)0)
#endif

namespace pipeann {

/** I/O context for attributing kernel I/O to logical workload. */
enum class IoContext : uint8_t {
  SEARCH = 0,
  PREFETCH = 1,
  INSERT = 2,
  COMPACTION = 3,
  OTHER = 4,
};

/** Thread-local current context (for eBPF: attribute I/O by TID → context). */
inline IoContext &observability_io_context() {
  static thread_local IoContext ctx = IoContext::OTHER;
  return ctx;
}

inline void set_io_context(IoContext ctx) {
  observability_io_context() = ctx;
#ifdef __linux__
  /* Thread names limited to 15 visible chars (16 with NUL); use short "pa:" prefix. */
  const char *name = "pa:other";
  switch (ctx) {
    case IoContext::SEARCH: name = "pa:search"; break;
    case IoContext::PREFETCH: name = "pa:prefetch"; break;
    case IoContext::INSERT: name = "pa:insert"; break;
    case IoContext::COMPACTION: name = "pa:compact"; break;
    default: break;
  }
  (void) pthread_setname_np(pthread_self(), name);
#endif
  /* Emit USDT so eBPF can see context transitions (TID + context_enum). */
  PIPANN_PROBE_IO_CONTEXT(static_cast<uint64_t>(ctx));
}

inline IoContext get_io_context() {
  return observability_io_context();
}

}  // namespace pipeann
