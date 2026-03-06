# How the New Run Works (overall_performance + setup)

This document explains how the BIGANN setup script and `overall_performance` test work, and **when the query counter is reset**.

---

## Overview

- **Setup script** (e.g. `setup_26fast_pipeann_bigann2.sh`): Prepares data, ground truth, and invokes `overall_performance`.
- **overall_performance**: Loads a disk index and query set, runs an initial search, then a loop of **batches**. Each batch: insert/delete a chunk of vectors, run **polling** searches while waiting, then run **Search after update** with the next ground-truth file.

---

## Environment Variables

| Variable | Set (example) | Unset (default) |
|----------|----------------|-----------------|
| **SCALE_M** | 10 (10M index) | 100 (100M) |
| **OVERALL_MAX_BATCHES** | 5 | 100 |
| **OVERALL_MAX_QUERIES** | 10000 or 20000 | No limit |
| **OVERALL_VECS_PER_STEP** | 10000 or 20000 | `index_npts/step` (e.g. 1M for 10M scale) |
| **S3_BASE** / **S3_QUERY** | S3 URIs | Use local data only |

- **OVERALL_MAX_QUERIES** caps how many queries are run **per batch** (during polling + “Search after update” for that batch). Each search run uses `query_num` queries (e.g. 10K from `bigann_query.bbin`).
- **OVERALL_VECS_PER_STEP** sets the insert/delete batch size and (when set) the ground-truth cadence for the test.

---

## Ground Truth

- **Default (OVERALL_VECS_PER_STEP unset):** 1M cadence; `num_steps = SCALE_M - 1` → e.g. gt_0, gt_1000000, … for 10M.
- **When OVERALL_VECS_PER_STEP is set:** Cadence = that value; `num_steps = OVERALL_MAX_BATCHES` (or 10).  
  Example: `OVERALL_VECS_PER_STEP=20000`, `OVERALL_MAX_BATCHES=5` → gt_0, gt_20000, gt_40000, gt_60000, gt_80000.

---

## Run Flow (overall_performance)

1. **Before any batch**
   - “Searching before inserts” with **gt_0**: two L values (e.g. L=20, L=30), each over all queries.  
   - This does **not** count toward the per-batch query cap.

2. **For each batch `i = 0 .. batch-1`:**
   - **Reset 1 (start of batch):** If `OVERALL_MAX_QUERIES > 0`, set `queries_this_phase = 0`.  
     So each batch starts with a fresh cap for **polling**.
   - Start async **insert** and **delete** of `vecs_per_step` vectors.
   - **Polling loop:** Every 5s, if insert/delete are not ready and the cap allows one more run (`queries_this_phase + total_queries + query_num <= max_queries`), run one search (e.g. 10K queries) with the **current** gt file and print “Queries processed: …”.  
     When insert/delete finish, exit the loop.
   - **After the loop:** Add `total_queries` to `queries_this_phase` (polling counts toward the cap).
   - **Switch gt file:** `res += vecs_per_step`, `currentFileName = GetTruthFileName(truthset_file, res)`.
   - **Reset 2 (new gt file):** Set `queries_this_phase = 0` so “Search after update” has room under the cap.
   - **Search after update:** For each L (e.g. 20, 30), run one search with `currentFileName`; if `queries_this_phase + query_num > max_queries` then stop (so with a 10K cap you get only one L per batch). After each run, add to `queries_this_phase` and print “Queries processed: …” when capped.
   - Optionally trigger merge, then next batch.

---

## When We Reset

| When | What is reset | Purpose |
|------|----------------|--------|
| **Start of each batch** | `queries_this_phase = 0` (if `max_queries > 0`) | So polling can run up to `OVERALL_MAX_QUERIES` during the insert/delete wait in **every** batch (you see “Queries processed” during the wait for all batches, not only the first). |
| **After updating `res` and switching gt file** | `queries_this_phase = 0` | So “Search after update” with the **new** gt file can run up to `OVERALL_MAX_QUERIES` (e.g. one or two 10K runs). |

So in each batch you get up to **OVERALL_MAX_QUERIES** during polling (with the previous batch’s gt file) and up to **OVERALL_MAX_QUERIES** in “Search after update” (with the new gt file).

---

## Example: Capped Run (5 batches, 20K queries per batch)

```bash
SCALE_M=10 OVERALL_MAX_BATCHES=5 OVERALL_MAX_QUERIES=20000 OVERALL_VECS_PER_STEP=20000 \
  S3_BASE=s3://bucket/bigann_base.bvecs S3_QUERY=s3://bucket/bigann_query.bbin \
  ./setup_26fast_pipeann_bigann2.sh
```

- Ground truth: gt_0, gt_20000, gt_40000, gt_60000, gt_80000 (20K cadence, 5 steps).
- Each batch: 20K insert + 20K delete; up to 20K queries in polling (e.g. “Queries processed: 10000”, “20000”) and up to 20K in “Search after update” (two L values).
- Total inserted: 5 × 20K = 100K.

---

## Example: Smaller Batches and Query Cap (5 batches, 10K per batch)

```bash
SCALE_M=10 OVERALL_MAX_BATCHES=5 OVERALL_MAX_QUERIES=10000 OVERALL_VECS_PER_STEP=10000 \
  S3_BASE=s3://bucket/bigann_base.bvecs S3_QUERY=s3://bucket/bigann_query.bbin \
  ./setup_26fast_pipeann_bigann2.sh
```

- Ground truth: gt_0, gt_10000, gt_20000, gt_30000, gt_40000 (10K cadence).
- Each batch: 10K insert + 10K delete; up to 10K in polling (one “Queries processed: 10000”) and up to 10K in “Search after update” (one L value only).
- Total inserted: 5 × 10K = 50K.

---

## Default Run (no caps)

```bash
# Unset caps to get default: 100 batches, 1M per batch, no query limit
unset OVERALL_MAX_BATCHES OVERALL_MAX_QUERIES OVERALL_VECS_PER_STEP
SCALE_M=100 ./setup_26fast_pipeann_bigann2.sh
```

- Batches: 100.  
- Vectors per batch: `index_npts/step` (e.g. 1M for 100M scale).  
- No query cap; polling and “Search after update” run fully every batch.  
- Ground truth at 1M cadence (when OVERALL_VECS_PER_STEP is unset).
