# FTS second optimization: unused scores and posting merges

This round reduces query work after the first submitted version `e91a37e3cac0f92a329d40a2dc7b715ebf510e40`, for which the user reported FTS raw score 13.525/100 (6.762/50 weighted). The platform supplied no per-metric timings or new task ID, so that score cannot independently identify a task SHA. The candidate product commit is `79f77cbb24fd7aa7627c37f7716dc9202cd11686`; its official score is still pending. The [first-round report](fts-performance-2026.md) remains a separate historical comparison against upstream. Reductions from the two experiments are not added together.

## Bottleneck and implementation

A separate diagnostic run on the original binary found BM25 evaluation, score filtering and DaaT merge operations in the approximate query window (1,654 PC samples). BM25 batch evaluation accounted for about 4.11% of these samples, simple-merger push 3.39%, score filtering 3.33%, collection 2.84%, and heap filling 2.60%. Phase boundaries were inferred from timestamps; this is diagnostic evidence, not an exact call-graph profile or a speedup measurement. Instrumentation was absent from all comparisons below.

For a natural-language MATCH used only as a bare WHERE truth predicate, the optimizer checks every other relation-expression scope and WHERE condition for score consumers. If none exist, the scan returns the document-ID union without calculating unused BM25 values or gathering the scoring statistics. This is the usual elimination of an unused numeric result; all postings, visibility checks and row lookups required for membership still execute. Numeric comparisons, projected scores, aggregate scores, implicit or explicit score ordering, Boolean mode and subqueries retain the original scoring path. Natural-language term weights are clamped positive by the existing BM25 implementation.

The previously dormant ID-only scan path now initializes its batch skip vector, handles plain table lookup trees, skips absent aggregate iterators, updates rescan ranges without dereferencing nonexistent scoring parameters, and accepts duplicate IDs in TaaT unions. Queries with at most 256 terms remain streaming DaaT scans, including ID-only scans; longer supported natural-language queries use bounded TaaT partitions. The relevance expression is not accessed when absent.

DaaT's factory constructs either the simple merger or loser tree. Its fill and collection wrappers choose that concrete type once per operation. Qualified template calls allow the compiler to inline heap operations instead of repeatedly invoking the virtual interface for each posting. Comparisons, tie handling, score aggregation and output order are preserved.

An experimental shortcut in shared string comparisons was withdrawn. The final charset, tokenizer and TOKENIZE expression code are byte-identical to the first scored submission. Existing first-round tokenizer and TaaT improvements remain in that submission. No index-format or migration changes are required.

## Complete measurements and variation

Both versions used the repository Release Clang 17 toolchain. The same saved first-submission binary ran three complete benchmarks, followed by three candidate benchmarks, followed immediately by three further benchmarks of that same saved old binary. Each run used all official defaults: 20,000 rows, batches of 500, 3,000 TOKENIZE repetitions, 200 query repetitions, three samples, 30 warmups, and `SKIP_LOAD=0`. None of the nine runs were dropped. Each value below is the arithmetic mean of three complete reports; build times are seconds and TOKENIZE/query times are milliseconds. Negative reductions are retained.

| Metric | Old before | Candidate | Old after | Reduction vs before | Reduction vs after |
| --- | ---: | ---: | ---: | ---: | ---: |
| `build_ik_all_sec` | 12.704667 | 13.547667 | 13.478333 | -6.64% | -0.51% |
| `build_ik_content_sec` | 9.921000 | 10.249000 | 10.145667 | -3.31% | -1.02% |
| `build_beng_en_sec` | 5.064000 | 5.718667 | 5.281333 | -12.93% | -8.28% |
| `tokenize_ik_avg_ms` | 0.268133 | 0.676100 | 0.546633 | -152.15% | -23.68% |
| `tokenize_beng_avg_ms` | 0.213500 | 0.434833 | 0.332467 | -103.67% | -30.79% |
| `query_cn_avg_ms` | 3.102067 | 2.700700 | 3.615133 | 12.94% | 25.29% |
| `query_beng_avg_ms` | 4.495700 | 3.815500 | 4.836533 | 15.13% | 21.11% |
| `query_mixed_avg_ms` | 3.341000 | 3.046567 | 3.779933 | 8.81% | 19.40% |
| `query_limit_avg_ms` | 3.736867 | 4.446900 | 4.296067 | -19.00% | -3.51% |

The categories are averaged internally and then given equal weight, exactly as in the task definition:

| Category | Reduction vs old before | Reduction vs old after |
| --- | ---: | ---: |
| build | -7.62% | -3.27% |
| tokenize | -127.91% | -27.24% |
| query | 4.47% | 15.57% |
| Equal-category mean | -43.69% | -4.98% |

The two old-binary phases have different control latency: mean SELECT 1 was 0.104000, 0.170200, and 0.167800 ms in execution order. The old binary also changed substantially in TOKENIZE time without a code or binary change. This prevents a reliable aggregate speedup or official-score forecast from these local runs. The earlier forward comparison is not hidden or replaced by the later comparison. No control latency was subtracted and no timing was corrected. Query reductions in both directions provide evidence for the intended query improvement; build and TOKENIZE movements should not be attributed to this round's query-only source changes.

Every complete run produced the same auxiliary hit counts: Chinese 8,001, BENG 11,000, mixed 7,332, LIMIT 20. [The JSON artifact](fts-performance-2026-round2.json) retains all nine parsed reports, both comparison directions, binary identities, server settings, asset hashes and regression results. These counts supplement the semantic checks below.

The host exposes 128 logical CPUs and about 503 GiB RAM. The runner, server, client and generator were all pinned to CPUs 4,5,6,7 (four distinct physical cores). Server parameters were `memory_budget=8G`, `cpu_count=4`, `datafile_size=4G`, `datafile_maxsize=8G`, and `log_disk_size=4G`. No exclusive host reservation or cgroup quota was applied. The server's `cpu_count` parameter alone does not impose CPU affinity.

| Binary | SHA256 |
| --- | --- |
| First scored submission `e91a37e3c` | `e9c28137b1469eba0372eee350c6952c6e2403c96a8b2c3b8e439780522b476c` |
| Candidate `79f77cbb2` | `a1e3dab45a219796bda85041579e202d1e82bf52269e3cfb4d9b60f4bb3697dc` |

The saved old binary's origin is recorded separately from the checkout used to launch the unchanged benchmark. In particular, the report's `git_head` field describes the benchmark checkout, not necessarily the running binary.

## Correctness

The final product patch completed the full 19-case native FTS suite: 17 passed and two retained failures previously reproduced on unmodified upstream. The full suite therefore returns failure status, rather than being described as fully passing.

- `basic_dml`, line 1598, reuses a stale table ID after dropping and recreating the table; `No such row` is inserted into SQL and causes syntax error 1064.
- `partitioned_simple_query` differs only in two preexisting EXPLAIN parameter indices (`:0` expected, `:1` actual). These expected values were preserved.

The new `membership_hotpath` expected results were generated on unchanged upstream `5bce9e06d1281347ad61344e4b01fab9158c58a2` before testing the candidate. It checks exact counts and ID sums across output batches, duplicate/missing terms, NULL and empty input, stopwords, LIMIT/OFFSET, projected numeric and Boolean results, scores rounded to ten decimal places, implicit/explicit ordering through the complete suite, thresholds including prepared parameters, SUM/HAVING, Boolean mode, NOT/OR, repeated prepared plans with more than 300 terms, EXISTS, and update/delete rollback.

`query_merge_hotpath` additionally covers dense/sparse results, top-k, score accumulation and a 302-distinct-term query. `parser_hotpath`, `tokenize_function`, `parser_properties` and `cn_word` cover the retained tokenizer paths. The new `collation_hotpath` oracle checks binary and Unicode ordering/grouping/equality, accents and trailing spaces, embedded NUL, binary IDs, joins and indexed range scans; its upstream-generated results also pass after withdrawing the comparison experiment.

Eight EXPLAIN blocks (four each in `simple_query.result` and `partitioned_simple_query.result`) now describe `calc_relevance=false` and removal of the unused score filter where applicable. Their SQL, row results and numeric results were not changed. The two existing mismatches were not edited away. Tests ran before the product patch was committed; its exact source patch was compared byte-for-byte with commit `79f77cbb24fd7aa7627c37f7716dc9202cd11686`. That commit then built from a clean working tree and completed all three candidate benchmarks.

Both old and new test instances exceeded the harness SIGTERM deadline and were stopped by the harness after SQL completion. Normal shutdown and restart were not validated. The new candidate has not yet received a formal competition score.

## Reproduction and protected assets

Use the same hardware, compiler, instance parameters and CPU affinity for both versions. For an already running dedicated test instance, run the official script unchanged, with a fresh output file for every full repetition:

```bash
export MYSQL='mysql -h127.0.0.1 -P2881 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2881 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

The script deletes and recreates its dedicated `fts_large_bench` database. Score-script output against the CI baseline on a different host is not an official competition score. No official workload SQL, generated data, timing logic, scoring rule or baseline was changed. These five assets are byte-identical to unmodified competition upstream:

| Unmodified official asset | SHA256 |
| --- | --- |
| `fts_large_bench.sh` | `596ea5d1c0038e666b78aa94bef22d2dfb87ffc9e84dbc7e2a1da1f746424462` |
| `fts_large_bench_baseline.json` | `f4e97eddb2e49c982b967fc8047f7cf37177eebe53d648f6746d85aacda91f0c` |
| `fts_large_bench_gen.py` | `b0c816dd8211ac0471f60b72a37a3fb5c62580357a5b6d7ac95f5a8c2e3f143d` |
| `fts_large_bench_score.py` | `bea52eb1c68d05693bb3ac5b910ab0e2fa0e70e01582f40466898ee1e2e3ef55` |
| `fts_large_bench_setup.sql` | `311826751df7f51ac45156fb4284aeca9bd9197370aceb88f4fb92832b458108` |
