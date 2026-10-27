# AMD CAGRA build acceleration

This directory documents the implementation and the reproducible benchmark method
for the **seekdb × AMD hipVS CAGRA** contest task. The code starts from the
frozen baseline tag:

- baseline repository: `https://github.com/zihaomu/seekdb`
- baseline tag: `amd-cagra-contest-baseline-v2`
- baseline commit: `58ddbd7459843e979a1a02f9bb4be9a3edacd3f0`
- submission branch: `amd-cagra-opt`

## What changed

### 1. Overlap the GPU CAGRA build with the CPU VSAG HNSW build

The relevant path is `src/oblib/lib/vector/ob_vsag_adaptor.cpp`.

For a `lib=cuvs` HNSW index, the baseline snapshot path does:

1. `add_index` buffers all vectors for the GPU;
2. `add_index` then builds the CPU VSAG HNSW index;
3. the first `knn_search` lazily builds CAGRA, so the CPU and GPU builds are
   serialized.

The change starts the GPU CAGRA build on a 32 MB-stack background pthread as
soon as a full bulk `add_index` snapshot is buffered, then lets the existing
VSAG CPU HNSW build run concurrently. The first query joins the background
thread. The measured build interval therefore still includes GPU completion;
no work is moved outside the timer.

The async path is only used for a full bulk add (`n == size`, at least 4096
vectors). Delta/incremental adds keep the original lazy build path, and
`lib=vsag` is untouched.

### 2. Reuse CAGRA search scratch memory

`tools/hipvs_bridge/seekdb_cuvs_bridge.c` previously allocated and freed three
RMM device buffers and created/destroyed `cuvsCagraSearchParams_t` on every
`seekdb_cuvs_search` call. Batch query benchmarks call this repeatedly with the
same shape. The bridge now:

- keeps query / id / distance device buffers per CAGRA handle and grows them on
  demand;
- keeps one search-params object per handle and raises `itopk_size` when a
  larger Top-K is requested.

The public result is still FP32 IDs/distances; only scratch allocation behavior
changed.

### 3. Reproducible build ablations

`seekdb_cuvs_bridge.c` accepts the following optional environment variables so
the build parameters can be measured without recompiling:

```text
SEEKDB_CUVS_GRAPH_DEGREE
SEEKDB_CUVS_INTERMEDIATE_DEGREE
SEEKDB_CUVS_NN_DESCENT_NITER
SEEKDB_CUVS_BUILD_ALGO=ivf_pq|nn_descent|auto
```

Defaults are exactly the hipVS/CAGRA defaults used by the frozen baseline
(intermediate 128, graph 64, IVF-PQ, 20 iterations). This keeps the default
candidate quality-compatible while allowing a documented ablation.

## Build

```bash
# 1. Build the bridge against the hipVS installation of the AMD image.
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

# 2. Build seekdb with the GPU path.
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=ON \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so \
  --make -j64
```

The trace build is useful for the required GPU-path evidence. It records
`cuvs_build` / `cuvs_serve` events to `OB_VSAG_TRACE_FILE` when
`OB_VSAG_TRACE=1`; the trace switch is compile-time only and is a no-op in the
default build.

## Correctness / quality gate

Run the baseline's self-contained gate after replacing the observer binary:

```bash
LD_LIBRARY_PATH=/work/bridge:/opt/hipvs/lib:/opt/rocm/lib \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh \
    --observer /path/to/trace-build/seekdb \
    --bridge /work/bridge/libseekdb_cuvs_bridge.so \
    --base-dir /tmp/seekdb-cagra-opt-smoke \
    --port 2981 \
    --render /dev/dri/renderD135 \
    --evidence /tmp/seekdb-cagra-opt-evidence
```

The gate covers GPU build/serve, VSAG isolation, non-L2 rejection, filter and
delete fallback, freshness rebuild, batch Top-10/Top-100 row cardinality, and
GPU-unavailable fallback. It must print `GPU smoke PASS`.

## Performance method

The official score is measured by the organiser with a fixed program. The local
reproduction for development is:

- same FP32 base vectors, dimensions, L2 metric and HNSW parameters as the
  frozen baseline;
- cold index and cold cache for every run;
- start timer when the raw table exists and the target index does not;
- stop timer after index creation, GPU synchronisation, and the first fixed
  query returning valid IDs;
- alternate baseline / candidate, at least 3 runs per dataset, use medians;
- compute median `CPU build / candidate build` and report Recall@10/100 against
  exact ground truth.

A starting point is the baseline smoke gate above (it already validates query
results and Recall for Top-10/Top-100) plus an external timing wrapper around
the DDL + first-query sequence. Use the optional env knobs above for a build
parameter ablation; report medians, not best runs.

## Evidence to collect in the cloud

- `OB_VSAG_TRACE` log with at least one `cuvs_build` and `cuvs_serve` per
  candidate run; this proves hipVS/CAGRA actually built and served.
- `nvidia-smi` equivalent (`rocm-smi`) showing the AMD GPU during the build.
- Three raw build times per version and the median speedup.
- Recall@10 and Recall@100 for CPU VSAG and GPU CAGRA, with the exact query
  set and ground truth.
- `lib=vsag` build/query smoke result, proving the CPU path is unchanged.

## Known constraints

- The local development machine used for this change has no AMD GPU/ROCm, so it
  cannot produce the official GPU timing numbers. The code is written to be
  measured on the contest AMD image; no timing number in this repository is
  presented as a cloud result.
- The async overlap is deliberately limited to a full bulk snapshot. Incremental
  delta adds continue to use the original lazy-build/fallback behavior.
- CAGRA remains FP32 at the public boundary; internal hipVS behavior is
  unchanged.
