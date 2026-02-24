# Observability with eBPF

This document describes how to observe the PipeANN query engine and kernel I/O path so you can answer **"why was this query slow?"** in terms of graph-walk, tier (cache vs disk), and SSD queueing.

## Overview: 3 layers


| Layer                         | Goal                                                                                  | Where to hook                                                                                                                           |
| ----------------------------- | ------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| **1. Physical data-plane**    | Break latency into cache hit/miss, NVMe queue time, device service time               | See **Layer 1** below: app tier_hit/tier_miss (per query) + kernel block tracepoints (total latency by comm); NVMe for queue vs device. |
| **2. Logical graph-walk**     | Why results were chosen: entry point, nodes expanded, pages touched, tier hits/misses | User: USDT/uprobes on PipeANN                                                                                                           |
| **3. Structural bottlenecks** | Hub pressure, dead ends, entry-point regret, drift                                    | Derived from (1)+(2)                                                                                                                    |


The **context tag** (SEARCH, INSERT, COMPACTION) is stamped so kernel I/O can be attributed to the right workload.

---

## Concepts and metrics

**Page** — The unit of data the search reads from disk or cache. One **page** = one **sector** = **4096 bytes** (`SECTOR_LEN` in the code). A page is a fixed-size block of the graph file: it holds several graph nodes and their neighbor lists laid out back-to-back. **page_id** is an index: “which 4 KB block of the graph?” (e.g. `page_id = byte_offset / 4096`). The search always reads whole pages; when it expands a node whose data lies on page 67313, it needs that page. If the page is in the in-process cache that’s a **tier hit**; if not, it’s a **tier miss** and the page is read from disk.

**queries** — Number of **query starts** (entry points). One query = one invocation of the search for a single vector (e.g. “find my 10 nearest neighbors”). The uprobe on the search function (e.g. `*pipe_search*`) fires once per query.

**expansions** — Total **node expansions**. The graph is searched by repeatedly “expanding” nodes: take a node, look at its neighbors, consider them for the result set. Each time the code expands a node (reads its neighbor list from a page), it fires `pipeann_observe_expand_node(node_id, page_id)`. **expansions** = how many nodes were opened across all queries.

**tier_hit** — **Cache hits**: number of times a page was found in the in-process cache. No disk I/O; the read is satisfied from memory.

**tier_miss** — **Cache misses**: number of times a page was *not* in cache and had to be **read from disk**. So **tier_miss** = number of disk reads (for a page). One query can cause many tier_miss events (each time it needs a page that isn’t cached); it’s not “one per query.”

**@by_page[page_id]** — In bpftrace scripts that do `@by_page[arg1]++` on `pipeann_observe_expand_node`, **arg1** is **page_id**. So **@by_page[page_id]** = number of **node expansions** that touched that page. Example: `@by_page[67313]: 24` means page 67313 was involved in 24 expansions (24 times we expanded a node whose data lives on that page). High values show which pages are “hot.”

**Per-query vs totals** — The scripts can report **totals** (e.g. total tier_miss over the run) or **per-query** counts (e.g. how many disk reads each query had). Per-query is done by assigning a query id when a search starts and keying events by that id (see **Per-query metrics** below).

---

## Context tagging (always on)

Every I/O path sets a **thread-local context** and (on Linux) renames the thread so eBPF can attribute I/O by TID or `comm`.

- `**pipeann::set_io_context(IoContext)`** — call at entry of each workload.
- `**pipeann::get_io_context()**` — current context (for optional USDT stamping).

**Contexts:** Thread names use a short `pa:` prefix so they fit Linux’s 15-character limit (16 with NUL). `set_io_context()` also emits the `**io_context` USDT probe** on every change so eBPF can see TID → context transitions without reading TLS.


| Enum         | Thread name (Linux) | Used in                                            |
| ------------ | ------------------- | -------------------------------------------------- |
| `SEARCH`     | `pa:search`         | page_search, pipe_search, beam_search, coro_search |
| `PREFETCH`   | `pa:prefetch`       | Reserved                                           |
| `INSERT`     | `pa:insert`         | bg_io_thread (writes for new nodes)                |
| `COMPACTION` | `pa:compact`        | merge_deletes                                      |
| `OTHER`      | `pa:other`          | Default                                            |


**Correlation:** Use the `io_context` probe (fired in `set_io_context()`) or key by `tid` / `comm`. For block I/O, see **Caveats** below — join by request pointer (`rq`), not just pid/tid.

---

## USDT probes (optional, build with `-DPIPANN_OBSERVABILITY`)

Build with `**-DPIPANN_OBSERVABILITY`** and systemtap/sdt headers (e.g. `systemtap-sdt-dev`) so the following probes are available. Without the define, probe macros are no-ops.

**Provider:** `pipeann`.


| Probe               | Arguments                   | When fired                                                                                                                                                      |
| ------------------- | --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `query_start`       | `(l_search, reserved)`      | Start of a search (page/pipe/beam). arg0 = l_search, arg1 = 0 (on some systems bpftrace cannot read USDT args; use a no-arg script to confirm the probe fires). |
| `query_done`        | `(total_us, n_ios, n_hops)` | End of search: total time (µs), I/O count, hop count.                                                                                                           |
| `expand_node`       | `(node_id, page_id)`        | When a node is expanded (page/sector requested).                                                                                                                |
| `read_page_request` | `(page_id, offset)`         | When a page is requested from disk (after tier miss).                                                                                                           |
| `tier_hit`          | `(page_id)`                 | Page served from in-process cache (byte-tier).                                                                                                                  |
| `tier_miss`         | `(page_id)`                 | Page not in cache; will go to disk.                                                                                                                             |
| `io_context`        | `(context_enum)`            | Fired in `set_io_context()` on every context change (0=SEARCH, 1=PREFETCH, 2=INSERT, 3=COMPACTION, 4=OTHER). Use for TID→context in eBPF.                       |


**Example (bpftrace):** USDT probes require the **path to the binary** that contains them (e.g. your search executable). Format is `usdt:BINARY:provider:probe`. List probes:

```bash
# Replace BINARY with the full path to the executable, e.g. build/tests/search_disk_index
sudo bpftrace -l 'usdt:BINARY:pipeann:*'
# Example from repo root:
sudo bpftrace -l 'usdt:/home/ubuntu/26FAST-PipeANN/build/tests/search_disk_index:pipeann:*'
```

Attach to query lifecycle and tier (use the same BINARY path in each probe):

```bash
# Replace BINARY with your search binary path (e.g. .../build/tests/search_disk_index)
# Query start/end and expansions
sudo bpftrace -e 'usdt:BINARY:pipeann:query_start { printf("query_start l_search=%lu\n", arg0); }'
sudo bpftrace -e 'usdt:BINARY:pipeann:query_done { printf("query_done us=%lu ios=%lu hops=%lu\n", arg0, arg1, arg2); }'
sudo bpftrace -e 'usdt:BINARY:pipeann:expand_node { printf("expand node_id=%lu page_id=%lu\n", arg0, arg1); }'
sudo bpftrace -e 'usdt:BINARY:pipeann:tier_hit { @hit[arg0]++; } usdt:BINARY:pipeann:tier_miss { @miss[arg0]++; }'
```

---

## Per-query graph-walk and tier metrics (uprobes on stubs)

The **Layer 2** goals (entry point, nodes expanded, pages touched, tier hits/misses) are not available via USDT in the current binary (see **Troubleshooting**). When built with `**-DPIPANN_OBSERVABILITY`**, the code also calls **observability stub functions** (C symbols) at the same sites. You can attach **uprobes** to these stubs to get the same semantics.

**Per-query stats (for each search):** For every search invocation we can measure:


| Metric             | Per-query map                                        | Meaning                                              |
| ------------------ | ---------------------------------------------------- | ---------------------------------------------------- |
| **Nodes expanded** | `@exp_per_query[query_id]`                           | Number of graph nodes expanded for this search.      |
| **Pages touched**  | `@tier_hit_per_query[id] + @tier_miss_per_query[id]` | Total page accesses (cache + disk) for this search.  |
| **Tier hits**      | `@tier_hit_per_query[query_id]`                      | Pages served from in-process cache for this search.  |
| **Tier misses**    | `@tier_miss_per_query[query_id]`                     | Pages read from disk (cache misses) for this search. |


The scripts below assign a query id on search entry and key all events by that id, so you get these four metrics per search. Every query id from 1 to N appears in each map (with 0 where there were no events).


| Stub (uprobe target)                | Arguments (arg0, arg1, …) | Meaning                                     |
| ----------------------------------- | ------------------------- | ------------------------------------------- |
| `pipeann_observe_expand_node`       | node_id, page_id          | Node expanded (page/sector requested).      |
| `pipeann_observe_tier_hit`          | page_id                   | Page served from in-process cache.          |
| `pipeann_observe_tier_miss`         | page_id                   | Page not in cache; will go to disk.         |
| `pipeann_observe_read_page_request` | page_id, offset           | Page requested from disk (after tier miss). |


### How per-query expansion, tier hit, and tier miss are implemented (code map)

This section ties the bpftrace per-query metrics to the exact source locations: where each event is emitted and how the tracer attributes it to a query.

**Data flow (summary)**

1. **Query id assignment** — When a search starts, the bpftrace script runs on the search entry probe (e.g. `uprobe:BINARY:*pipe_search`*). It increments a global counter `@nquery` and stores the current query id for this thread: `@query_id[tid] = @nquery`. So the Nth search invocation gets query id N (1-based).
2. **Per-query map setup** — On that same probe the script initializes `@exp_per_query[@nquery]`, `@tier_miss_per_query[@nquery]`, and `@tier_hit_per_query[@nquery]` to 0 for that new query id.
3. **Attributing events** — When the process calls `pipeann_observe_expand_node`, `pipeann_observe_tier_miss`, or `pipeann_observe_tier_hit`, the bpftrace handler runs in the same thread. The script uses `@query_id[tid]` to get the current query id and increments the corresponding per-query map (e.g. `@exp_per_query[@query_id[tid]]++`). So every expansion and every tier hit/miss is counted for the query currently running on that thread.

**Application-side: where the probes are defined and fired**


| What                                  | File                                                                               | Location                                                            | Description                                                                                                                                                                                                                                                                           |
| ------------------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Probe macros and stub declarations    | [include/observability.h](include/observability.h)                                 | Lines 39–58 (macros), 28–33 (extern "C" stubs)                      | `PIPANN_PROBE_QUERY_START`, `PIPANN_PROBE_EXPAND_NODE`, `PIPANN_PROBE_TIER_HIT`, `PIPANN_PROBE_TIER_MISS` each emit USDT and call the corresponding `pipeann_observe_`* stub.                                                                                                         |
| Stub implementations (uprobe targets) | [src/observability_stubs.cpp](src/observability_stubs.cpp)                         | Lines 21–37                                                         | Empty functions `pipeann_observe_expand_node`, `pipeann_observe_tier_hit`, `pipeann_observe_tier_miss`, `pipeann_observe_read_page_request`. bpftrace attaches uprobes here and reads `arg0`/`arg1`.                                                                                  |
| Query start (pipe search)             | [src/search/pipe_search.cpp](src/search/pipe_search.cpp)                           | Line 48                                                             | Start of `pipe_search`: `PIPANN_PROBE_QUERY_START(l_search)`. One fire per search invocation.                                                                                                                                                                                         |
| Query start (page search)             | [src/search/page_search.cpp](src/search/page_search.cpp)                           | Line 109                                                            | `PIPANN_PROBE_QUERY_START(l_search)` at entry of `page_search`.                                                                                                                                                                                                                       |
| Query start (beam search)             | [src/search/beam_search.cpp](src/search/beam_search.cpp)                           | Line 32                                                             | `PIPANN_PROBE_QUERY_START(l_search)` at entry of beam search.                                                                                                                                                                                                                         |
| Expand node (pipe search)             | [src/search/pipe_search.cpp](src/search/pipe_search.cpp)                           | Line 209                                                            | Inside `send_read_req` lambda: when sending a read for a neighbor, `PIPANN_PROBE_EXPAND_NODE(item.id, pid)`. One fire per node expansion.                                                                                                                                             |
| Expand node (page search)             | [src/search/page_search.cpp](src/search/page_search.cpp)                           | Line 282                                                            | When locking the frontier and preparing page reads: `PIPANN_PROBE_EXPAND_NODE(id, page_id)` per frontier node.                                                                                                                                                                        |
| Expand node (beam search)             | [src/search/beam_search.cpp](src/search/beam_search.cpp)                           | Line 150                                                            | When expanding a node and requesting its page: `PIPANN_PROBE_EXPAND_NODE(id, page_id)`.                                                                                                                                                                                               |
| Tier hit / tier miss                  | [src/utils/linux_aligned_file_reader.cpp](src/utils/linux_aligned_file_reader.cpp) | Lines 491–499 (single read), 516–522 (batch), 528 (READ_ONLY_TESTS) | In `send_read_no_alloc`: if `v2::cache.get(page_id, buf)` returns true the page is served from cache and `PIPANN_PROBE_TIER_HIT(page_id)` fires; else `PIPANN_PROBE_TIER_MISS(page_id)` fires and the read goes to disk. So tier_miss = cache miss = disk read, tier_hit = cache hit. |


**Why per-query attribution works** — Search runs with one thread (or a bounded pool) per logical query. When the code expands a node or the file reader reports a tier hit/miss, that runs in the same thread as the query that triggered the work. So `tid` in bpftrace identifies the current query for that thread, and `@query_id[tid]` maps thread id to the query id assigned at search entry. The per-query maps accumulate expansion count and tier hit/miss count per query id.

**Order of events for one query** — Typically: (1) search entry → probe fires, bpftrace assigns query id and zeros per-query slots; (2) for each node the search expands, `PIPANN_PROBE_EXPAND_NODE` fires and the read is sent; (3) when the I/O layer handles that read it checks the cache and fires either `PIPANN_PROBE_TIER_HIT` or `PIPANN_PROBE_TIER_MISS`. So for one expansion you get one expand_node and then one tier_hit or one tier_miss. The per-query script counts all of these keyed by the same query id.

**Exact code references (file:line)**

- **Probe macros and stubs** — [include/observability.h](include/observability.h): `PIPANN_PROBE_QUERY_START` (line 40), `PIPANN_PROBE_EXPAND_NODE` (44–46), `PIPANN_PROBE_TIER_HIT` (51–53), `PIPANN_PROBE_TIER_MISS` (55–57); stub declarations (30–32).
- **Stub bodies (uprobe targets)** — [src/observability_stubs.cpp](src/observability_stubs.cpp): `pipeann_observe_expand_node` (21–25), `pipeann_observe_tier_hit` (26–29), `pipeann_observe_tier_miss` (30–33).
- **Query start** — [src/search/pipe_search.cpp](src/search/pipe_search.cpp):48, [src/search/page_search.cpp](src/search/page_search.cpp):109, [src/search/beam_search.cpp](src/search/beam_search.cpp):32.
- **Expand node** — [src/search/pipe_search.cpp](src/search/pipe_search.cpp):209, [src/search/page_search.cpp](src/search/page_search.cpp):282, [src/search/beam_search.cpp](src/search/beam_search.cpp):150.
- **Tier hit/miss** — [src/utils/linux_aligned_file_reader.cpp](src/utils/linux_aligned_file_reader.cpp):492 (tier_miss), 496 (tier_hit), 500 (read-only path); batch path 516–520, 528.

Use the **full path** to the executable as `BINARY`. Example (replace the `-c "..."` with your full search command if needed):

```bash
export BINARY=$(pwd)/build/tests/search_disk_index

# Nodes expanded (node_id, page_id) — one line per expansion
sudo bpftrace -e 'uprobe:'$BINARY':pipeann_observe_expand_node { printf("expand node_id=%lu page_id=%lu\n", arg0, arg1); }' \
  -c "$BINARY float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40"

# Tier hit vs miss counts (print every 5s)
sudo bpftrace -e 'uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit++; } uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss++; } interval:s:5 { printf("tier_hit=%lu tier_miss=%lu\n", @tier_hit, @tier_miss); }' \
  -c "$BINARY float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40"

# Combined: query count + expansions + tier hit/miss (summary every 5s).
# queries_with_0_expansions = queries - expansions (invocations that never called expand_node).
sudo bpftrace -e '
  uprobe:'$BINARY':*pipe_search* { @queries++; }
  uprobe:'$BINARY':pipeann_observe_expand_node { @expansions++; @by_page[arg1]++; }
  uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss++; }
  interval:s:5 {
    printf("queries=%lu expansions=%lu tier_hit=%lu tier_miss=%lu queries_0_exp=%lu\n",
      @queries, @expansions, @tier_hit, @tier_miss, @queries - @expansions);
  }
' -c "$BINARY float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40"
```

**Example output** (combined script with `interval:s:5`). You see periodic lines from bpftrace; the search program’s own log and benchmark table are interleaved. **queries_0_exp** is the number of search invocations with zero node expansions (queries − expansions). When the run ends, bpftrace exits and (if you also print maps in END or on exit) you see the `@by_page` histogram:

```
queries=26261146 expansions=5637380 tier_hit=0 tier_miss=5637380 queries_0_exp=20623766
queries=27618861 expansions=5806611 tier_hit=0 tier_miss=5806611 queries_0_exp=21812250
queries=28903564 expansions=5980210 tier_hit=0 tier_miss=5980209 queries_0_exp=22923354
queries=30190678 expansions=6154035 tier_hit=0 tier_miss=6154035 queries_0_exp=24036643
    40          32      572.11     1701.40     2713.00        0.00       59.38       91.54
[ssd_index.cpp:65:INFO] Lock table size: 0
[ssd_index.cpp:66:INFO] Page cache size: 0

@by_page[87083]: 2
@by_page[146763]: 2
@by_page[90758]: 2
@by_page[34872]: 2
@by_page[5793]: 2
@by_page[140339]: 4
@by_page[11578]: 4
@by_page[129969]: 4
@by_page[50423]: 4
@by_page[17177]: 4
@by_page[335]: 4
@by_page[179402]: 4
```

Here `tier_hit=0` means no pages were served from the in-process cache (e.g. cache disabled or cold); every expansion caused a tier_miss (disk read). The `@by_page` entries show how many expansions touched each page (e.g. page 140339 was expanded into 4 times). To see `@by_page` when the run ends, add `END { print(@by_page); }` to the combined script.

**What the maps mean:**

- **@by_page[page_id]** — Number of **node expansions** that touched that page. `arg1` in `pipeann_observe_expand_node` is the page (sector) id. So `@by_page[67313]: 24` means page 67313 was involved in 24 expansions (24 times we expanded a node whose data lives on that page). High values show which pages are hot.

**How many disk reads per query** — To get disk reads **per query** instead of a single total, **count tier_miss per query** in bpftrace: (1) assign a query id when a search starts (e.g. on `*pipe_search`* entry), (2) on each `pipeann_observe_tier_miss` fire, increment a map keyed by that query id. Each map entry is then “query id → number of disk reads for that query.” Same idea applies to **expansions per query** and tier_hit per query: key those events by the same query id.

**Per-query expansions and tier_miss (each query → count)** — The script below assigns a query id on search entry and keys both expansions and tier_miss by that id, so you get **for each query** how many expansions and how many disk reads. With `-c`, some bpftrace builds do not run `END`; if you get no output, run the search in the background and attach with `-p $!` (see second script).

Minimal example (disk reads per query only; END prints the map). **Every query gets an entry** (0 if no tier_miss):

```bash
sudo bpftrace -e '
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @tier_miss_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  END { printf("disk reads per query (query_id -> count):\n"); print(@tier_miss_per_query); }
' -c "$BINARY float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40"
```

**Per-query metrics (disk reads, expansions, tier hit):** Full script that also counts expansions and tier_hit per query. **Every query is present in all three maps** (value 0 if no expansion / tier_miss / tier_hit):

```bash
# For each query: expansions and tier_miss. Every query id gets an entry (0 or more).
sudo bpftrace -e '
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @exp_per_query[@nquery] = 0;
    @tier_miss_per_query[@nquery] = 0;
    @tier_hit_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_expand_node { @exp_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit_per_query[@query_id[tid]]++; }
  END {
    printf("=== expansions per query (query_id -> count) ===\n"); print(@exp_per_query);
    printf("=== tier_miss (disk reads) per query (query_id -> count) ===\n"); print(@tier_miss_per_query);
    printf("=== tier_hit per query ===\n"); print(@tier_hit_per_query);
  }
' -c "$BINARY float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40"
```

**Pages touched per query** = `@tier_hit_per_query[id] + @tier_miss_per_query[id]` (total page accesses for that search). No separate map is needed; derive it when analyzing the output.

**If `END` does not run** (e.g. you see `Could not resolve symbol: /proc/self/exe:END_trigger` when using `-c`), use one of these:

**Option A — Attach to the process** (start search in background, then attach). On some systems END still does not run when the process exits; if you see `END_trigger` again, use Option B.

```bash
export BINARY=$(pwd)/build/tests/search_disk_index
./build/tests/search_disk_index float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40 &
sudo bpftrace -p $! -e '
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @exp_per_query[@nquery] = 0;
    @tier_miss_per_query[@nquery] = 0;
    @tier_hit_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_expand_node { @exp_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit_per_query[@query_id[tid]]++; }
  END {
    printf("=== expansions per query ===\n"); print(@exp_per_query);
    printf("=== tier_miss (disk reads) per query ===\n"); print(@tier_miss_per_query);
    printf("=== tier_hit per query ===\n"); print(@tier_hit_per_query);
  }
'
```

**Option B — Print on an interval** (works with or without `-c`; no END needed). Prints the three maps every N seconds; use the last snapshot as your per-query result. Use a short interval (e.g. 15–30 s) for runs that finish in a few minutes:

```bash
export BINARY=$(pwd)/build/tests/search_disk_index
sudo bpftrace -e '
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @exp_per_query[@nquery] = 0;
    @tier_miss_per_query[@nquery] = 0;
    @tier_hit_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_expand_node { @exp_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit_per_query[@query_id[tid]]++; }
  interval:s:30 {
    printf("\n=== per-query snapshot ===\n");
    printf("Total queries: 1 to %lu (map key = query id, 1-based)\n", @nquery);
    printf("=== expansions per query (query_id -> count) ===\n"); print(@exp_per_query);
    printf("=== tier_miss per query (query_id -> count) ===\n"); print(@tier_miss_per_query);
    printf("=== tier_hit per query (query_id -> count) ===\n"); print(@tier_hit_per_query);
  }
' -c "$BINARY float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40"
```

Example: `@exp_per_query[42]: 120` = query 42 had 120 expansions; `@tier_miss_per_query[42]: 5` = query 42 had 5 disk reads. **Every query id from 1 to N appears in both maps** — queries with no expansions show `@exp_per_query[id]: 0`, and queries with no tier_miss show `@tier_miss_per_query[id]: 0`, so you can see the total number of queries and how many had 0 expansions or 0 disk reads.

**Important:** The map key **is** the **query id** (same as `@nquery` when that search started): the Nth time the search entry point (`*pipe_search*`) was entered gets id N (1-based). So `@exp_per_query[144]: 5` means query id 144 had 5 expansions. Total queries = number of keys in the map = max key value. The keys are exact query ids (1, 2, 3, …); they are **not** document or vector IDs. If you see a smaller range (e.g. 0–4095) than `@nquery`, the snapshot may be from an earlier interval, or the run had fewer queries at print time; the header line prints “Total queries: 1 to %lu” so you see the exact range for that snapshot.

**Large runs (100K+ queries):** When total queries is very large, bpftrace’s `print(@exp_per_query)` (and the other per-query maps) may **only output a subset** of the keys (e.g. you see ids 1–4096 or a few thousand lines even though the header says "Total queries: 1 to 1224571"). That is a bpftrace output limit/truncation: the maps hold all query ids 1 to N, but `print()` does not necessarily dump every key. The header still gives the **exact query id range** (1 to @nquery). For full stats over 1M+ queries, use the **distribution** script (how many queries had 0, 1, 2, … expansions / tier_miss) instead of the full per-query map, or run with fewer queries if you need every query id listed.

**Fixing "only 4096 ids" and "only 1000 entries shown" for 60K+ queries:** (1) **Map size:** bpftrace's default `max_map_keys` is **4096**, so each per-query map stores at most 4096 keys; query ids beyond that are dropped. Set `**export BPFTRACE_MAX_MAP_KEYS=100000`** (or larger) and run with `**sudo -E bpftrace ...**` so the env is preserved — then all 60600 query ids are stored. (2) **Print limit:** `print(@exp_per_query)` may only display ~1000 entries; that is a display truncation. The map still holds all keys up to `max_map_keys`. For full aggregate stats use the **distribution** script. Example for siftsmall (60600 queries):

```bash
export BINARY=$(pwd)/build/tests/search_disk_index
export BPFTRACE_MAX_MAP_KEYS=100000
sudo -E bpftrace -e '
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @exp_per_query[@nquery] = 0;
    @tier_miss_per_query[@nquery] = 0;
    @tier_hit_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_expand_node { @exp_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit_per_query[@query_id[tid]]++; }
  interval:s:1 {
    printf("\n=== per-query snapshot ===\n");
    printf("Total queries: 1 to %lu (map key = query id, 1-based)\n", @nquery);
    printf("=== expansions per query (query_id -> count) ===\n"); print(@exp_per_query);
    printf("=== tier_miss per query (query_id -> count) ===\n"); print(@tier_miss_per_query);
    printf("=== tier_hit per query (query_id -> count) ===\n"); print(@tier_hit_per_query);
  }
' -c "$BINARY float /mnt/nvme/indices/siftsmall/siftsmall 1 32 /mnt/nvme/data/siftsmall/siftsmall_query.bin /mnt/nvme/data/siftsmall/siftsmall_gt.bin 10 l2 2 0 10 20 30 40"
```

**What to expect:** In some setups (e.g. small L, cold cache, or beam/page search with low width) you can see **about 1 expansion and 1 disk read per query** — one page is read to get the initial neighborhood, and the rest is in cache or not needed. So `@exp_per_query[id]: 1` and `@tier_miss_per_query[id]: 1` for many queries is normal. Larger L or cache misses will show more expansions and tier_miss per query.

**Writing per-query data to a file (e.g. .txt):** Redirect bpftrace stdout to a file so you capture all printed output. The script below uses `**print()`** and works on **all bpftrace versions**; redirect with `**> per_query.txt`**. Note: `print()` only outputs a subset of entries (~1000), so the file will contain at most that many lines per map plus the "Total queries" header (which still shows the true total, e.g. 60600).

```bash
export BINARY=$(pwd)/build/tests/search_disk_index
export BPFTRACE_MAX_MAP_KEYS=150000
sudo -E bpftrace -e '
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @exp_per_query[@nquery] = 0;
    @tier_miss_per_query[@nquery] = 0;
    @tier_hit_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_expand_node { @exp_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit_per_query[@query_id[tid]]++; }
  interval:s:1 {
    printf("\n=== per-query snapshot (write to file with > per_query.txt) ===\n");
    printf("Total queries: 1 to %lu (map key = query id, 1-based)\n", @nquery);
    printf("=== expansions per query ===\n"); print(@exp_per_query);
    printf("=== tier_miss per query ===\n"); print(@tier_miss_per_query);
    printf("=== tier_hit per query ===\n"); print(@tier_hit_per_query);
  }
' -c "$BINARY float /mnt/nvme/indices/siftsmall/siftsmall 1 32 /mnt/nvme/data/siftsmall/siftsmall_query.bin /mnt/nvme/data/siftsmall/siftsmall_gt.bin 10 l2 2 0 10 20 30 40" > per_query.txt
```

With `**> per_query.txt**` you get one snapshot per interval; use the last snapshot in the file. The **"Total queries: 1 to N"** line is exact; the printed map entries are truncated (~1000).

**Full dump (bpftrace 0.24+):** If your bpftrace supports `**for ($kv : @map)`**, use the command below to write **one line per query** and get all entries in the file. Check version with `bpftrace --version` (need 0.24 or newer). If you see `**syntax error, unexpected for`**, your version does not support it.

**Required:** If you see **"Map full; can't update element"** (retcode -7), the per-query maps hit the default 4096-key limit. The script below sets `**config = { max_map_keys=100000; }`** in the preamble so every map gets a 100k-key limit from the script itself (no env var or sudo -E needed). If you see **"Lost N events"**, the ring buffer is too small; the command sets `BPFTRACE_PERF_RB_PAGES=65536` and uses `interval:s:30`. Use the last snapshot in `per_query.txt`.

```bash
export BINARY=$(pwd)/build/tests/search_disk_index
export BPFTRACE_PERF_RB_PAGES=65536
sudo -E bpftrace -e '
  config = { max_map_keys=100000; }
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @exp_per_query[@nquery] = 0;
    @tier_miss_per_query[@nquery] = 0;
    @tier_hit_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_expand_node { @exp_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_hit { @tier_hit_per_query[@query_id[tid]]++; }
  interval:s:30 {
    printf("query_id\texpansions\ttier_miss\ttier_hit\n");
    for ($kv : @exp_per_query) {
      printf("%lu\t%lu\t%lu\t%lu\n", $kv.0, $kv.1, @tier_miss_per_query[$kv.0], @tier_hit_per_query[$kv.0]);
    }
    printf("total_queries\t%lu\n", @nquery);
  }
' -c "$BINARY float /mnt/nvme/indices/siftsmall/siftsmall 1 32 /mnt/nvme/data/siftsmall/siftsmall_query.bin /mnt/nvme/data/siftsmall/siftsmall_gt.bin 10 l2 2 0 10 20 30 40" > per_query.txt
```

For a **guaranteed** full dump on any system, use **application-side logging** (binary writes CSV when e.g. `PIPEANN_STATS_FILE` is set).

**Comparing runs: totals vs per-query**

- **Totals script** (e.g. `queries=459584 expansions=197450 tier_hit=0 tier_miss=197450 queries_0_exp=262134`):  
  - **expansions ≈ tier_miss** when **tier_hit=0** — every expansion needed a page that wasn’t in cache, so each expansion caused one disk read. That’s normal for a cold cache or first pass.  
  - **queries_0_exp** (or **queries − expansions**) is the number of **pipe_search** invocations that had **zero expansions**. The script prints this as `queries_0_exp` every interval so you see it directly (e.g. 262,134). That can happen because: (1) **warmup** runs that call the search path without doing a full graph walk, (2) multiple **L** values (e.g. 10, 20, 30, 40) so each “user query” is run several times and some code paths may not expand, (3) early exits or different algorithm branches that don’t call `expand_node`. So “queries” = number of times the search entry point was hit; “expansions” = number of times a node was actually expanded.
- **Distribution script** (e.g. `@dist_expansions[0]: 14954111`, `@dist_expansions[1]: 4096`):  
  - Shows **how many queries** had 0, 1, 2, … expansions (or disk reads). So a large count at 0 means many query invocations had no expansions; that matches the totals picture (queries > expansions).

**Distribution (how many queries had 0, 1, 2, … disk reads or expansions):** Use a return probe to histogram per-query counts. **With `-c`, bpftrace often does not run `END`** (error: `Could not resolve symbol: /proc/self/exe:END_trigger`), so you get no output if you rely on END. Use **interval** to print the distribution every N seconds instead:

```bash
# Distribution printed every 10s (works with -c; END often fails with -c).
# Every query gets exp/tier_miss entries (0 or more) so distribution counts are correct.
sudo bpftrace -e '
  uprobe:'$BINARY':*pipe_search* {
    @query_id[tid] = ++@nquery;
    @exp_per_query[@nquery] = 0;
    @tier_miss_per_query[@nquery] = 0;
  }
  uprobe:'$BINARY':pipeann_observe_expand_node { @exp_per_query[@query_id[tid]]++; }
  uprobe:'$BINARY':pipeann_observe_tier_miss { @tier_miss_per_query[@query_id[tid]]++; }
  uretprobe:'$BINARY':*pipe_search* {
    @dist_expansions[@exp_per_query[@query_id[tid]]]++;
    @dist_tier_miss[@tier_miss_per_query[@query_id[tid]]]++;
  }
  interval:s:10 {
    printf("\n=== # queries by expansion count (so far) ===\n"); print(@dist_expansions);
    printf("=== # queries by disk read (tier_miss) count (so far) ===\n"); print(@dist_tier_miss);
  }
' -c "$BINARY float /mnt/nvme/indices/sift/sift 1 32 /mnt/nvme/data/sift/sift_query.bin /mnt/nvme/data/sift/sift_gt.bin 10 l2 2 0 10 20 30 40"
```

You get a snapshot every 10 seconds; the last snapshot before the run finishes is your cumulative distribution. Example: `@dist_tier_miss[5]: 1000` means 1000 queries had exactly 5 disk reads.

**Alternative (final print when process exits):** Run the search in the background and attach with `-p $!`; then use the same script but with `END { ... }` instead of `interval`. When the target process exits, bpftrace may run END and print the maps once.

**Entry point:** Use the existing uprobe on the search function (e.g. `*pipe_search*` for pipe search) as “query start”; the stub probes above give you expansions and tier behavior per query (aggregated across all threads).

---

## Layer 1: Breaking latency into cache hit/miss, NVMe queue time, device service time

The Overview (3 layers) calls out **Layer 1**: break latency into **cache hit/miss**, **NVMe queue time**, and **device service time**. Below we define those and how to measure them.

### Definitions


| Component               | Meaning                                                                               | Where measured                                                                                                                                                                                          |
| ----------------------- | ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Cache hit/miss**      | Read satisfied from in-process cache (hit) vs had to go to disk (miss).               | **App:** `pipeann_observe_tier_hit` / `pipeann_observe_tier_miss` — already **per query** (tier_hit_per_query, tier_miss_per_query). Tier hit = no block I/O; each tier_miss becomes one block request. |
| **NVMe queue time**     | Time the request waits in the block/NVMe queue before the device starts servicing it. | **Kernel:** Requires NVMe-level tracepoints; block layer alone does not split queue vs device.                                                                                                          |
| **Device service time** | Time the device takes to complete the I/O.                                            | **Kernel:** With block tracepoints: **total block I/O latency** (issue to complete). With NVMe tracepoints: from dispatch to device until complete.                                                     |


So: **per-query** cache hit/miss is from existing tier scripts. **Total block I/O latency** per request uses block tracepoints. **Queue vs device time** needs NVMe (or similar) tracepoints.

### Kernel hook points (physical data-plane)

Use these to measure **where time went** (cache vs queue vs device).


| Hook                                                   | Purpose                                                                       |
| ------------------------------------------------------ | ----------------------------------------------------------------------------- |
| **block:block_rq_issue** / **block:block_rq_complete** | Per-request total latency; join by `rq`; at issue store timestamp and `comm`. |
| **VFS / filemap**                                      | Proxies for cache hit/miss (e.g. did read go to disk).                        |
| **io_uring:*** (if used)                               | Submission/completion timings for async I/O.                                  |
| **nvme:*** (optional)                                  | Queue time vs device time (e.g. `sudo bpftrace -l 'tracepoint:nvme:*'`).      |


**Correlation:** Join by `**rq`** (request pointer), not pid/tid (completion can run on another thread). At **issue** store `comm` (e.g. pa:search) to attribute latency by workload. Thread name is `pa:search` / `pa:insert` / `pa:compact`. Use the `**io_context` USDT probe** (fired in `set_io_context()`) to map TID → context in eBPF.

### How the block I/O latency script works (what to expect)

**What we measure:** For each **block I/O request** (read or write that goes to the block layer), we measure **one number**: the time from when the request is **issued** to the block layer until it **completes**. That is “total block I/O latency” in microseconds (it includes any time in the block/NVMe queue plus device service; we do not split queue vs device in this script).

**Two kernel events:** The kernel fires:

1. **block_rq_issue** — when a request is handed to the block layer (on its way to the device). We record **timestamp** and the **thread name (comm)** of the task that issued it.
2. **block_rq_complete** — when that request has finished. We look up the timestamp we stored for this request and compute **latency = now − then**.

To do that we must **match** each complete to the right issue. The tracepoint payload does not give us the request pointer; it gives **device (dev)** and **sector**. So we use **(dev, sector)** as the key: at issue we store `@start[dev, sector] = time` and `@comm[dev, sector] = comm`; at complete we look up by (dev, sector), compute latency, and add it to a **histogram per comm** (so we see “how many requests from thread X had latency in bucket Y”). Then we delete that key so the same sector can be reused.

**Why “by comm”:** The **comm** (thread name) is read at **issue** time. So the histogram is “block I/O latency for I/O that was issued by this thread name.” That lets you see, for example, that 35,000 requests issued by `iou-sqp-253931` had latency 64–128 µs.

**Why you see iou-sqp-**** instead of pa:search:** Your search uses **io_uring** for async I/O. The thread that actually calls into the block layer (and triggers `block_rq_issue`) is the **io_uring kernel thread** (e.g. `iou-sqp-253931`), not your application thread (`pa:search`). The PID in the comm (253931) is the process that owns the io_uring context — your search process. So **iou-sqp-253931** *is* your search’s disk I/O; you just see it under the kernel thread name.

**What to expect when you run it:**

1. **Start the bpftrace script** (root). It attaches to the two block tracepoints and prints a snapshot every 5 seconds. Initially you may see only **jbd2**, **kworker**, or **node** — other activity on the same disk.
2. **Start your search** (same machine). While it is doing disk I/O, **iou-sqp-**** will appear in the snapshots. The PID will be your search process. You’ll see a histogram: most requests often in a band (e.g. 64–128 µs on NVMe), with a tail to higher latencies.**
3. **Stop the search.** After a short while, new snapshots will no longer have (or will have very few) requests for that iou-sqp comm, because that process is no longer issuing I/O.
4. Each printed block is a **fresh 5-second window** (the script clears the histogram every 5s), so you see a rolling view of “who did I/O and how long it took.”

**Summary:** The script answers “for each thread name that issued block I/O, how many requests fell into each latency bucket (µs) in the last 5 seconds?” When your search is running, its I/O shows up under **iou-sqp-****; that histogram is what to look at for “how fast are my search’s disk reads.”**

### Block I/O latency script (total latency per request, by comm)

Block tracepoints expose **dev** and **sector** (not the raw request pointer `rq`) in the tracepoint payload. Use `(dev, sector)` as the key to join issue with complete. Store issuer `comm` at issue time. Run as root:

```bash
sudo bpftrace -e '
  tracepoint:block:block_rq_issue
  {
    @start[args->dev, args->sector] = nsecs;
    @comm[args->dev, args->sector] = comm;
  }
  tracepoint:block:block_rq_complete
  /@start[args->dev, args->sector]/
  {
    $lat_us = (nsecs - @start[args->dev, args->sector]) / 1000;
    @lat_by_comm[@comm[args->dev, args->sector]] = hist($lat_us);
    delete(@start, (args->dev, args->sector));
    delete(@comm, (args->dev, args->sector));
  }
  interval:s:5
  {
    printf("\n=== Block I/O latency (us) by comm ===\n");
    print(@lat_by_comm);
    clear(@lat_by_comm);
  }
'
```

If your kernel uses **dot** for tracepoint args (e.g. `args.dev`, `args.sector`), replace `args->dev` / `args->sector` with `args.dev` / `args.sector`. Check with `sudo bpftrace -lv tracepoint:block:block_rq_issue`.

### Understanding the block I/O latency output

**What you see:** The script prints a snapshot every **5 seconds**. Each snapshot is introduced by:

`=== Block I/O latency (us) by comm ===`

Then one **histogram per comm** (thread name that issued the I/O), in the form:

`@lat_by_comm[comm_name]:`  
followed by lines like:

`[min, max)    count | bar`

- **[min, max)** — Latency bucket in **microseconds** (µs). For example `[64, 128)` means “≥ 64 µs and < 128 µs”.
- **count** — Number of block I/O requests in that bucket in the last 5 seconds.
- **| bar** — Same count drawn as a bar (relative to the largest count in that histogram).

**Important: the buckets are latency (time), not request size.** Each bucket is a **time range in microseconds (µs)**. The count is "how many requests **took** that long to complete," not how many bytes were read or written. So [4, 8) with count 1 means "1 request had latency between 4 and 8 µs"; [64, 128) with count 10096 means "10,096 requests had latency between 64 and 128 µs." Disk read/write **sizes** (sectors, bytes) are not bucketed in this script.

**Why the bucket boundaries are 4, 8, 16, 32, 64, 128, 256, 512:** The script uses bpftrace's `**hist()`** builtin. `hist()` chooses bucket boundaries on a **power-of-2 (logarithmic) scale**, not linear. So you get small buckets at low **latency** values ([4, 8), [8, 16), … µs) and wider buckets at high latencies ([64, 128), [128, 256), … µs). That gives good resolution where most requests fall (e.g. 64–128 µs on NVMe) and keeps the tail (slower requests) in a few buckets. You cannot change these boundaries in the script; they are fixed by bpftrace.

**Example:** For `@lat_by_comm[iou-sqp-253931]`:

```
[64, 128)          35230 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[128, 256)          6022 |@@@@@@@@                                            |
```

means: in that 5-second window, **35,230** requests had latency between 64 and 128 µs, and **6,022** between 128 and 256 µs. Most of your app’s I/O is in the 64–128 µs range.

**What each comm means:**


| comm                                      | Meaning                                                                                                                                                                                                                                                                  |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **iou-sqp-****                            | io_uring submission-queue poll thread for process . When your app uses io_uring (e.g. PipeANN search), the kernel thread that **issues** block I/O has this name. The PID is your process (e.g. search_disk_index). **This is your search workload’s disk I/O latency.** |
| **jbd2/nvme0n1p1-**                       | Journal thread for the NVMe partition (e.g. ext4 journal). Metadata writes; name is truncated (Linux comm is 15 chars).                                                                                                                                                  |
| **journal-offline**                       | Offline journal work (e.g. fsck or journal replay).                                                                                                                                                                                                                      |
| **kworker/u32:0**, **kworker/0:1H**, etc. | Kernel worker threads. They can show up as the “issuer” when the block layer or a driver uses workqueues to issue or complete I/O. So some of this I/O may still be on behalf of your app (e.g. completions), but the **comm** is the worker, not the app.               |
| **node**                                  | Often a Node.js or other runtime process name. If you have other apps (e.g. a web server) doing disk I/O, they appear here.                                                                                                                                              |


**Important:** The script **clears** the histogram every 5 seconds, so each block is a **rolling 5-second window**: only I/O that completed in that window is counted. When your search is running, look for the snapshots that contain **iou-sqp-****; that histogram is the block-layer latency for your search. When the search has finished, those snapshots will no longer include iou-sqp (or the count will drop to zero).**

### Combining Layer 1 with per-query

- **Per-query cache hit/miss:** Use the existing per-query script (Full dump bpftrace 0.24+): `@tier_hit_per_query[query_id]`, `@tier_miss_per_query[query_id]` — that is Layer 1 cache breakdown **per query**.
- **Block latency by workload:** Use the block script above for **total block I/O latency by comm** (pa:search, pa:insert, pa:compact). That gives search vs other workload latency; the kernel has no query id, so this is per **context** not per query.
- **Per-query block latency:** Would require the app to tag each I/O with query id; without that you get (1) per-query tier (cache) stats and (2) per-context block latency. Together: “how many cache misses per query” and “how slow are search’s disk I/Os on average.”

### Getting the full Layer 1 breakdown: cache hit/miss, queue time, device time

**What you have today:**


| Component                                | How you get it                                                                                                                                                              |
| ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Cache hit/miss**                       | **Already have it.** Per-query tier script: `@tier_hit_per_query[query_id]`, `@tier_miss_per_query[query_id]`. Tier hit = no block I/O. Each tier_miss = one block request. |
| **Total block latency** (queue + device) | Block script above: `block_rq_issue` → `block_rq_complete`. This is **queue + device combined**, not device-only.                                                           |
| **NVMe queue time**                      | Not in block script; need NVMe tracepoints or blktrace.                                                                                                                     |
| **Device service time**                  | Not in block script; need NVMe tracepoints (driver submit → device complete).                                                                                               |


**Splitting queue vs device:** Block gives one number (total). **Device service time** = from when the NVMe driver submits the command to the device until it completes. Use **NVMe tracepoints**: `nvme:nvme_setup_cmd` (command submitted) and `nvme:nvme_complete_rq` (completed). Then device time = complete − setup (join by request/command id; run `sudo bpftrace -lv tracepoint:nvme:nvme_setup_cmd` to see args). **Queue time** = total (from block script) − device time, for the same I/O (correlating block and NVMe events per request is kernel-dependent). **Alternative:** use **blktrace** + **btt** for a full breakdown without custom correlation.

**Device service time (NVMe script).** Join `nvme_setup_cmd` and `nvme_complete_rq` by **(ctrl_id, qid, cid)**. Your kernel exposes:

- **nvme_setup_cmd:** disk, ctrl_id, qid, opcode, flags, fctype, **cid**, nsid, metadata, cdw10  
- **nvme_complete_rq:** disk, ctrl_id, qid, **cid**, result, retries, flags, status

Run as root (device-time histogram every 5s):

```bash
sudo bpftrace -e '
  tracepoint:nvme:nvme_setup_cmd
  {
    @start[args->ctrl_id, args->qid, args->cid] = nsecs;
  }
  tracepoint:nvme:nvme_complete_rq
  /@start[args->ctrl_id, args->qid, (uint64)args->cid]/
  {
    $device_us = (nsecs - @start[args->ctrl_id, args->qid, (uint64)args->cid]) / 1000;
    @device_us = hist($device_us);
    delete(@start, (args->ctrl_id, args->qid, (uint64)args->cid));
  }
  interval:s:5
  {
    printf("\n=== NVMe device service time (us) ===\n");
    print(@device_us);
    clear(@device_us);
  }
'
```

If your kernel uses **dot** (e.g. `args.ctrl_id`), use that instead of `args->`. **Buckets:** Same as the block script — bpftrace `hist()` uses the same power-of-2 scale (e.g. [64, 128), [128, 256), [256, 512) µs), so you can compare total and device histograms directly.

**Combined script: total (block) + device (NVMe) in one run.** One bpftrace process, same 5-second window, both histograms printed together:

```bash
sudo bpftrace -e '
  tracepoint:block:block_rq_issue
  {
    @b_start[args->dev, args->sector] = nsecs;
    @b_comm[args->dev, args->sector] = comm;
  }
  tracepoint:block:block_rq_complete
  /@b_start[args->dev, args->sector]/
  {
    $lat_us = (nsecs - @b_start[args->dev, args->sector]) / 1000;
    @lat_by_comm[@b_comm[args->dev, args->sector]] = hist($lat_us);
    delete(@b_start, (args->dev, args->sector));
    delete(@b_comm, (args->dev, args->sector));
  }
  tracepoint:nvme:nvme_setup_cmd
  {
    @n_start[args->ctrl_id, args->qid, args->cid] = nsecs;
  }
  tracepoint:nvme:nvme_complete_rq
  /@n_start[args->ctrl_id, args->qid, (uint64)args->cid]/
  {
    $device_us = (nsecs - @n_start[args->ctrl_id, args->qid, (uint64)args->cid]) / 1000;
    @device_us = hist($device_us);
    delete(@n_start, (args->ctrl_id, args->qid, (uint64)args->cid));
  }
  interval:s:5
  {
    printf("\n=== Block I/O latency (us) by comm (total = queue + device) ===\n");
    print(@lat_by_comm);
    clear(@lat_by_comm);
    printf("\n=== NVMe device service time (us) ===\n");
    print(@device_us);
    clear(@device_us);
  }
'
```

Block map names are prefixed with `b_` and NVMe with `n_` so they don’t clash. You get total (by comm) and device (all NVMe) every 5s; approximate queue ≈ total − device in aggregate.

**NVMe queue time.** Block tracepoints use (dev, sector); NVMe use (ctrl_id, qid, cid). There is no common key in the tracepoint payloads, so you **cannot** correlate the same I/O from block to NVMe in bpftrace and compute queue time per request. You can still:

- **Compare in aggregate:** Run the **block script** (total) and this **NVMe script** (device) at the same time. Average queue time ≈ (average total from block) − (average device from NVMe). You get two histograms, not a per-request queue histogram.
- **Use blktrace + btt** for a full per-request breakdown (queue vs device) if you need it.

### Queue time vs device service time (optional; reference)

Block tracepoints give **one number per request** (issue → complete). To split **queue time** vs **device service time**: use **NVMe tracepoints** (see above) or **blktrace** / **btt**. See kernel `include/trace/events/nvme.h` or `sudo bpftrace -l 'tracepoint:nvme:*'`.

**Example (conceptual):** “p99 rose because COMPACTION consumed 70% of NVMe queue depth for 2s” → filter block events by `comm == "pa:compact"` and measure queue depth / bandwidth over time.

---

## Caveats (eBPF / block I/O)

- **Join block I/O by request pointer, not just pid/tid.** Completion can happen on a different execution context; PID/TID on `block_rq_complete` may not match the issuer. On both tracepoints the kernel exposes `struct request *rq` — use it as the key: on `block_rq_issue` store (rq → context/timestamp); on `block_rq_complete` look up by `rq` for latency and context.
- `**comm` is a label, not a unique key.** Thread names are length-limited (15 chars), can change, and are not unique. Prefer `(pid, tid)` + timestamps + `rq` for joins.
- **Async I/O / helper threads:** If submission or completion runs on worker threads, “current” context may not reflect the original query. Rely on USDT (`query_start`, `expand_node`, `read_page_request`, `tier_hit`/`tier_miss`) for semantic anchors.

---

## What you can report (per context)

With context + kernel events + optional USDT:

**Per context (SEARCH, PREFETCH, INSERT, COMPACTION):**

- Outstanding I/O (queue depth) over time  
- IOPS / bandwidth  
- Latency histograms (p50 / p95 / p99)  
- Top block offsets / page IDs (hot adjacency)  
- Wait vs service time

**Per query (with USDT):**

- Graph steps: expansions, pages touched  
- Tier hits vs misses (and which page_ids missed)  
- NVMe time: queued + serviced  
- “Why slow”: e.g. cache misses on hub pages, compaction interference

---

## Code map (where context and probes are set)


| Location                                  | Context                     | Probes                                                       |
| ----------------------------------------- | --------------------------- | ------------------------------------------------------------ |
| `src/search/page_search.cpp`              | SEARCH at entry             | query_start, expand_node, query_done                         |
| `src/search/pipe_search.cpp`              | SEARCH at entry             | query_start, expand_node, query_done                         |
| `src/search/beam_search.cpp`              | SEARCH in do_beam_search    | query_start, expand_node, query_done                         |
| `src/search/coro_search.cpp`              | SEARCH at entry             | (context only)                                               |
| `src/utils/linux_aligned_file_reader.cpp` | (uses thread context)       | tier_hit, tier_miss, read_page_request in send_read_no_alloc |
| `src/update/direct_insert.cpp`            | INSERT in bg_io_thread      | —                                                            |
| `src/update/delete_merge.cpp`             | COMPACTION in merge_deletes | —                                                            |


---

## Build

- **Context + thread naming:** No extra build flags; include `observability.h` where used (already done in the paths above).
- **USDT probes:** Require `-DPIPANN_OBSERVABILITY` and the `sys/sdt.h` header (e.g. from `systemtap-sdt-dev`). If the header is missing, the build still succeeds and probes are no-ops.

### Steps to enable and use USDT probes

1. **Install the SDT header** (Debian/Ubuntu):
  ```bash
   sudo apt-get install -y systemtap-sdt-dev
  ```
2. **Reconfigure and build with observability enabled:**
  ```bash
   cd build && rm CMakeCache.txt && cmake .. -DPIPANN_OBSERVABILITY=ON && make -j4
  ```
   If you use a wrapper script that passes `-DPIPANN_OBSERVABILITY=ON` via `ADDITIONAL_DEFINITIONS`, run that after installing `systemtap-sdt-dev` and clear the CMake cache so the header is detected.
3. **Confirm the binary has USDT.** You can check in two ways:
  **When you run search_disk_index:** The program prints one line at startup:
  - `[observability] USDT probes: enabled (attach with bpftrace)` — built with USDT; you can attach bpftrace.
  - `[observability] USDT probes: disabled (...)` — build without USDT or without sys/sdt.h; probes are no-ops.
   **Without running (no root):** Inspect the binary for USDT (SystemTap) notes:
   You will typically see **only** `pipeann` / `io_context`. To list probe names with bpftrace (requires root):
   With the current build (static linking), only `**io_context`** is present in the final executable; see **Troubleshooting** below.
4. **Attach to probes.** Use the **full path** to the executable (`BINARY`). With the current build only `**io_context`** is in the binary (see **Troubleshooting**). Two ways to get events:
  **Option A — Two terminals (attach by PID):** USDT by path alone does not attach to processes that start *after* bpftrace. Start the search first, then attach bpftrace with `-p <pid>`. **Timing matters:** the search runs load → warmup → benchmark then exits. If you attach bpftrace too late (e.g. after typing in another terminal), most or all queries may already have run and you will see no events before the process exits. Attach **immediately** after starting the search:
   Run from the **repo root** (so `./build/tests/search_disk_index` and `$BINARY` are correct). `$!` is the PID of the background job; bpftrace attaches immediately. When the search process exits, bpftrace exits too. Results remain in `/tmp/search_out.log`.
   **If you see "couldn't get argument 0" (or "argument 1"):** Some toolchains don't emit USDT argument locations bpftrace can read. Use a **no-arg** script to confirm the probe fires: `sudo bpftrace -p $! -e "usdt:$BINARY:pipeann:io_context { @++; } END { print(@); }"`.
   **If you see "Attaching 1 probe..." but no events and no count:** The search process may exit before bpftrace attaches (e.g. `sudo` delay), or END may not run when the target exits. Use **Option B** below (`-c`) so bpftrace *starts* the search and attaches before it runs — then you always get a count when the child exits.
   **Option B — Single command:** Run the search binary **directly** as the child of bpftrace with `-c` (no bash wrapper). The **child process must be the binary that contains the USDT probes** (search_disk_index); if you use `-c "bash ..."`, the child is bash and bpftrace reports "No probes to attach". So pass the search binary and its args as the full `-c` argument:
   You should see `Attaching 1 probe...` then many `io_context` lines. **During a search-only run you will see only `io_context=0`** (0 = SEARCH); that is expected. You would see 2 (INSERT) or 3 (COMPACTION) only if the same process ran inserts or merge_deletes. To capture search output separately, run the search in the background and use Option A; otherwise output mixes with the search program’s log and table. To capture search output separately, run the search in the background and use Option A (`bpftrace -p $! ...`) in the same shell. If you see **"Error finding location for probe: ... query_done"**, the binary has multiple `query_done` sites (one per search backend); use the one-probe version above or **Option A** (two terminals). Use your own paths and L values (the trailing `10 20 30 40` are the L-search list). With `mem_L=0` the disk index is used only and no `*_mem.index` file is needed.
   Example one-liners (with `BINARY=$(pwd)/build/tests/search_disk_index` set):
   **Alternative (attach to running process):** Start the search in the background, then attach; when you Ctrl+C bpftrace, it will print the map with the total count:

### Troubleshooting: "No probes to attach"

If you see **"No probes to attach"** when using `usdt:$BINARY:pipeann:query_start` (or `query_done`, `expand_node`, etc.):

- **Cause:** The search code lives in a **static library**. When the executable is linked, GNU ld merges `.note.stapsdt` sections from the archive; in practice only **one** USDT note (the `io_context` probe) is present in the final binary. So `bpftrace -l 'usdt:...:pipeann:*'` shows only `io_context`, and attachment to `query_start` fails.
- **What works:** Use the `**io_context`** probe (context transitions: SEARCH=0, INSERT=2, COMPACTION=3). Use **uprobes** for query-level events, e.g. `uprobe:$BINARY:*pipe_search*` for pipe search entry.
- **Check:** Run the program and look for `[observability] USDT probes: enabled`. Run `readelf -n build/tests/search_disk_index | grep -A2 stapsdt` — you will see only `io_context`. Use `BINARY` as the **full path** to the executable and attach to `pipeann:io_context`, not `pipeann:query_start`.

### Troubleshooting: "No output" / "Could not resolve symbol: END_trigger"

When using `**-c`** (bpftrace runs the search as a child), `**END { ... }` often does not run** and you may see `ERROR: Could not resolve symbol: /proc/self/exe:END_trigger`. So scripts that only print in END produce no output.

- **Workaround:** Use `**interval:s:N`** to print maps every N seconds instead of END (e.g. distribution script above uses `interval:s:10`). You get periodic snapshots; the last one before the run ends is effectively the final result.
- **Alternative:** Run the search in the background and attach with `**-p $!`**; then END may run when the target process exits and you get one final print.



sys/sdt.h comes from the SystemTap SDT (statically defined tracing) dev package. Install it, then reconfigure so CMake can find it.

sudo apt-get install -y systemtap-sdt-dev

After installing: CMake caches the “have sys/sdt.h” check, so reconfigure and rebuild:
cd /home/ubuntu/26FAST-PipeANN/build
rm -f CMakeCache.txt
cmake .. -DPIPANN_OBSERVABILITY=ON
make -j$(nproc)

cd /home/ubuntu/26FAST-PipeANN/build
rm -f CMakeCache.txt
cmake .. -DPIPANN_OBSERVABILITY=ON
make -j$(nproc)

cd /home/ubuntu/26FAST-PipeANN
rm -f build/CMakeCache.txt
cmake -B build -DPIPANN_OBSERVABILITY=ON
cmake --build build -j$(nproc)