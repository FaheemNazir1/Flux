# Flux benchmarks

`flux_bench` measures indexing throughput, thread scaling, search latency and where indexing time goes. Everything runs on a synthetic corpus that is generated locally from a fixed seed; nothing is downloaded.

> **Always benchmark an optimized build.** A build with no `CMAKE_BUILD_TYPE` has no optimization flags and its timings are meaningless (`flux_bench` prints a warning). Use a separate Release build directory so your normal `build/` stays as it is.

## Quick start (PowerShell, MSYS2 UCRT64 + MinGW Makefiles)

```powershell
$env:Path = "C:\Program Files\CMake\bin;C:\msys64\ucrt64\bin;$env:Path"
cmake -S . -B build-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 8

# 1. Generate data (once). Presets: small ~0.1M tokens, medium ~2M tokens, large ~12M tokens.
.\build-release\flux_bench.exe generate --out benchmarks\data\medium --preset medium

# 2. Indexing throughput and thread scaling (database off and on)
.\build-release\flux_bench.exe index --data benchmarks\data\medium --threads 1,2,4,8,12 --db both --reps 5 --warmup 1 --csv benchmarks\results\index.csv

# 3. Search latency
.\build-release\flux_bench.exe search --data benchmarks\data\medium --samples 200

# 4. Where does single-threaded indexing time go?
.\build-release\flux_bench.exe stages --data benchmarks\data\medium --docs 2000
```

The `medium` run of step 2 takes about 5 minutes on the reference machine below. Use `--preset small` for a quick check (a few seconds, but too short for stable timings). `flux_bench info` prints only the environment block.

Other options: `generate` accepts `--files --words --vocab --seed --dirs` to override the preset; `index` accepts `--db on|off|both` and `--work-dir DIR` (where temporary databases are created; default: the system temp folder).

## Dataset generator

- **Deterministic.** Output depends only on `(files, words, vocab, seed, dirs)`. The PRNG (SplitMix64) and the integer Zipf weights are fully specified, so the same settings give byte-identical files on any platform, and file *i* does not depend on the order files are generated.
- **Realistic skew.** Words are drawn with Zipf-like frequencies: rank 1 appears in almost every document, high ranks in very few. Text has capitalization, commas, sentences and paragraphs, so the tokenizer is exercised.
- **Known answers.** `.flux_dataset` (written last) records the exact word count. Flux must report exactly that many tokens and exactly `files` documents; the benchmark aborts its result with exit code 3 otherwise.
- **Safe.** The generator refuses to write into any non-empty directory, never overwrites or deletes a file, and reuses a directory only if its manifest has identical settings. Output defaults to `benchmarks/data/`, which is git-ignored and entirely separate from `sample_docs/`. The manifest is the only extra file (Flux counts it as 1 unsupported file).

Generation time is printed but is never part of any measurement.

## What is measured

### `index`

| Item | Meaning |
|---|---|
| **timed region** | one `indexDirectory()` call: directory scan + file reads + tokenization, and with a database also the writer queue, batched transactions and the final commit |
| **not timed** | creating the database (`open`: file, schema, WAL setup) and closing it (WAL checkpoint). Both are recorded separately in the CSV (`open_s`, `close_s`) |
| **db off / on** | `off`: nothing is persisted. `on`: a fresh, empty database file for every run, deleted afterwards |
| **median s / min s / max s** | over the timed repetitions of that configuration |
| **docs/s, tokens/s** | documents (tokens) processed divided by the **median** time |
| **speedup** | median time at the first listed thread count ÷ median time here, same db mode |

Correctness gate: every run (including warm-ups) must report the same documents, tokens, empty/unsupported/skipped counts, equal to the manifest, and in `db on` mode the same database contents (documents, terms, postings, token sum) as every other run. Any difference is printed as `MISMATCH` and the tool exits with 3.

### `search`

The database is built first (not timed), then reopened read-only. Queries are chosen by Zipf rank so their selectivity is known. The timed region is one `searchIndex()` call (query tokenization, statement preparation, binding and fetching up to 20 rows, as the CLI does). `matches` is the total number of matching documents from a separate untimed run. Median, min and max are reported in microseconds; **p95 uses the nearest-rank method and is shown only with at least 20 samples**.

### `stages`

Single-threaded cost of each pipeline stage on the same documents (read, tokenize, build per-document term frequencies, database write through the real `IndexWriter`). Use it to see which stage limits parallel speedup, not to predict absolute throughput.

### `sync`

What the desktop app and `flux sync` really run, end to end: directory listing alone, then `syncDirectory()` into a fresh temporary database (a **first sync**) and a second sync of the unchanged tree, in two modes (`content` = only the supported text types; `names` = the Milestone 10 default, which also records the names of every other file), at each thread count; then filename+content search against the result, side by side with the old content-only `searchIndex()`.

```powershell
.\build-release\flux_bench.exe sync --data benchmarks\data\medium --threads 1,4,12 --reps 3 --other-files 50000
```

`--other-files N` additionally generates N tiny non-text files (`file_<i>.dat` in 100 folders) inside the private scratch directory (never in your data or the repository) to measure the name-only path; the tree is removed afterwards. Correctness gate: the second sync must find every file unchanged. Do not run other work while it runs.

## How to compare thread counts fairly

1. Same dataset, same build, same machine state: put all thread counts in **one** `index` invocation. Configurations run in rotating order within each repetition so no thread count always runs first or last.
2. Every run starts from an equivalent state: an empty database, or none.
3. Discard a warm-up pass (default 1) and use the **median** of several repetitions (default 5).
4. Treat differences smaller than the min–max spread as noise.
5. Compare results across sessions only with great care (see variability below). To compare two builds, run them alternately in the same session, as done for the A/B test below.
6. Close other heavy programs; do not run builds or tests while benchmarking.

## Reference machine and conditions

Observed from Windows: AMD Ryzen 5 5600H (6 cores / 12 threads), 7.3 GB RAM, SSD, Windows 11. GCC 16.2.0 (MSYS2 UCRT64), CMake Release (`-O3 -DNDEBUG`), SQLite 3.54.1 (vendored), dataset on `D:`, temporary databases in the user's temp folder on `C:`. Dataset `medium`: 5000 files, 2,000,014 tokens, 13.7 MiB, vocabulary 10,000, seed 42.

## Results

All numbers below were produced by the commands above on the machine above; raw files are in the git-ignored `benchmarks/results/`. Wall-clock seconds, median of 5 repetitions after 1 discarded warm-up pass, `medium` dataset.

### Indexing: before and after the optimizations

Before = the code as of Milestone 3 (SQLite default page cache, 200 documents per transaction, immediate posting inserts). After = final code.

| db | threads | before median s | after median s |
|---|---|---|---|
| off | 1 | 2.044 | 1.898 |
| off | 2 | 1.226 | 1.118 |
| off | 4 | 0.719 | 0.684 |
| off | 8 | 0.485 | 0.537 |
| off | 12 | 0.500 | 0.547 |
| **on** | **1** | **42.17** (34.1–56.0) | **8.68** (8.37–8.75) |
| on | 2 | 44.55 | 7.33 |
| on | 4 | 48.40 | 6.78 |
| on | 8 | 45.94 | 6.87 |
| on | 12 | 46.39 | 6.85 |

With the database off the code did not change, and the differences above are within run-to-run spread. With the database on, wall time fell roughly 5x (1 thread) to 7x (12 threads). Final throughput at 12 threads: 9,147 docs/s and 3.66M tokens/s without a database; 730 docs/s and 292k tokens/s with one. Final thread scaling: without a database 3.5x at 8 threads, flat beyond that; with a database 1.28x at 4 threads and flat beyond.

### Search latency (final code, 5000 documents, limit 20)

| query | matches | median µs | p95 µs |
|---|---|---|---|
| rank 1 | 5000 | 3948 | 6753 |
| rank 10 | 4857 | 4047 | 6774 |
| rank 100 | 1676 | 1952 | 2744 |
| rank 1000 | 213 | 345 | 555 |
| rank 10000 | 18 | 114 | 258 |
| AND rank 1+10 | 4857 | 14586 | 16620 |
| AND rank 10+100 | 1647 | 10031 | 11266 |
| AND rank 1+10+100 | 1647 | 16350 | 18150 |
| no match | 0 | 116 | 159 |

Latency grows with the number of postings a query must aggregate. No search optimization was attempted: nothing measured here showed search to be a problem.

### Where single-threaded time goes (2000 documents, 480,581 postings)

| stage | before s | after s | share after |
|---|---|---|---|
| read files | 0.465 | 0.446 | 16.5% |
| tokenize | 0.119 | 0.112 | 4.1% |
| build term frequencies | 0.189 | 0.178 | 6.6% |
| **database write** | **9.835** | **1.968** | **72.8%** |

(An intermediate stage run between the two, with only the cache and batch changes, gave 3.40 s.)

### Milestone 5 regression check (same `medium` dataset, same method)

Milestone 5 changed code shared with plain indexing: the directory scanner, per-file stat calls, and the writer's task queue. To check for a regression, the Milestone 4 `flux_bench` binary and the final one were run alternately in the same session (order old, new, new, old; medians of 5 after a warm-up, seconds):

| db | threads | Milestone 4 (2 runs) | final (2 runs) |
|---|---|---|---|
| off | 1 | 1.67, 1.83 | 1.55, 1.57 |
| off | 12 | 0.45, 0.52 | 0.39, 0.40 |
| on | 1 | 8.56, 8.49 | 8.72, 8.52 |
| on | 12 | 7.07, 6.99 | 6.94, 7.04 |

With the database on, the difference is within run-to-run spread. Without it, the final code is faster (about 11% at 1 thread and 20% at 12 threads), because the rewritten directory scanner is cheaper: 23.4 µs/file versus 34.5 µs/file for the old one in five alternating scan measurements. One repetition in the third position stalled for about 24 s (an outlier; medians are unaffected).

How this was found: a first version of the Milestone 5 code guarded every read with three extra `stat` calls and queried each directory entry three times; the same A/B showed 25–35% slower runs without a database. Both were fixed: the guard now costs one extra `stat` call and runs only when a document is going to be stored, and the scanner makes one status query per entry.

### Milestone 6 regression check

Milestone 6 added a `root_id` column and index to `documents`, per-root bookkeeping in the writer, and further scanner changes (link and system-folder checks). The final Milestone 5 binary and the Milestone 6 binary were run alternately (order old, new, new, old; same `medium` dataset, medians of 5 after a warm-up, seconds):

| db | threads | Milestone 5 (2 runs) | Milestone 6 (2 runs) |
|---|---|---|---|
| off | 1 | 1.69, 1.64 | 1.65, 1.29 |
| off | 12 | 0.35, 0.39 | 0.33, 0.31 |
| on | 1 | 7.82, 8.57 | 6.94, 6.43 |
| on | 12 | 6.77, 6.86 | 5.58, 5.21 |

No regression was found; Milestone 6 was equal or somewhat faster in every configuration. The cause of the improvement was not investigated, and with the session-to-session variability described below it should not be read as a speed-up claim. Each run's correctness check (document and token counts) passed.

## Bottleneck analysis and optimizations

**Finding.** With a database, indexing was limited by the single SQLite writer: 92.7% of single-threaded time was spent in the database write stage, and adding worker threads made no difference (0.87x–1.00x). Reading, tokenizing and term counting were never the limit.

**Experiments** (throwaway programs, 1000–5000 documents, same dataset):

| change | effect measured |
|---|---|
| in-memory database instead of a file | ~4x cheaper per posting: disk I/O was not the main cost |
| SQLite page cache 2 MiB → 64 MiB | 18.6 → 6.1 µs/posting at 2000 documents (~3x) |
| commit batch 200 → 1000 documents | ~20–25% cheaper (50 → 9.5 µs, 200 → 5.8–6.5, 1000 → 4.7 µs/posting) |
| `foreign_keys=OFF` | ~14% cheaper — **not adopted**, integrity checking stays on |
| dropping the `postings_by_doc` index | ~33% cheaper — **not adopted**, it keeps re-indexing deletes fast |
| rebuilding SQLite with `THREADSAFE=2`, `MEMSTATUS=0`, etc. | 5.81 vs 5.76 µs/posting: no measurable effect, **not adopted** |
| insert each batch's postings sorted by (term, document) | 13–17% cheaper in isolation |

**Optimizations in the code.**

1. `PRAGMA cache_size = -65536` (64 MiB) on read-write connections ([src/database.cpp](../src/database.cpp)). SQLite's default cache was far smaller than the pages a batch of random-order inserts touches, so dirty pages spilled to the WAL and were re-read.
2. Default writer batch 200 → 1000 documents ([include/flux/index_writer.hpp](../include/flux/index_writer.hpp)). Each commit has a substantial fixed cost. A failure now rolls back at most 1000 documents, never corrupts; re-running replaces them.
3. Postings are buffered and inserted sorted by (term, document) just before each commit ([src/index_writer.cpp](../src/index_writer.cpp)). If a path repeats within one batch, pending postings are flushed first so replacement stays correct (tested).

The dedicated writer thread, prepared statements and transaction safety are unchanged.

**Evidence for (3), an alternating A/B in one session** (db on, medians of 5, seconds; "A" = optimizations 1+2 only, "B" = 1+2+3):

| threads | round 1 A / B | round 2 A / B |
|---|---|---|
| 1 | 6.99 / 7.22 | 8.36 / 8.57 |
| 4 | 6.92 / 5.80 | 7.73 / 7.04 |

At 1 thread sorting made no measurable difference; at 4 threads, where the writer is saturated, B was faster in both rounds (16% and 9%; the second round's ranges overlap). The honest summary: a modest gain, roughly 10–15%, when the writer is the bottleneck. It is kept because the default thread count (12) is that case.

## Remaining bottleneck

Database writing is still ~73% of single-threaded time, and with a database the throughput stops improving at about 4 threads. About 6–8 µs per posting remains, spent in two B-tree inserts plus foreign-key checks per posting. Further gains would require a different storage design (for example building in-memory segments and merging them, or a different postings layout), which is an architectural change not justified by this milestone's evidence.

## Variability and limitations

- **Session-to-session drift is large.** The same binary measured 9.65 s in one run and 7.0–8.4 s in later runs, about 20% apart. Only compare numbers from the same session, or differences much larger than that. The before/after gap in the table (5–7x) is far larger than this drift.
- **File cache is uncontrolled.** The OS file cache is not flushed. Warm-up runs are discarded, so reported runs are effectively warm. Cold-cache behaviour was not measured.
- **Other uncontrolled variables:** CPU frequency/thermal behaviour of a laptop CPU, background processes and antivirus, power plan, and the database living on a different drive than the dataset.
- **One machine, one compiler, one dataset family.** Results say nothing about Linux, other disks, or real-world corpora with different term distributions.
- **Scan is single-threaded** in Flux and timed together with processing; `stages` reports it separately (about 50 µs per file).
- **Synthetic text.** Words are pronounceable pseudo-words with Zipf frequencies; real text has different vocabulary growth.
- Search latency was measured on a freshly written database with a warm SQLite cache.
- Peak memory use was not measured.

## Content extraction per format (`extract_bench`)

`extract_bench` measures how fast each document format is read and tokenized, on documents it generates itself (fixed sizes, no randomness, written only to a private temporary folder that is removed afterwards; no database is involved):

```powershell
.\build-release\extract_bench.exe --files 24 --threads 4 --rows 20000
```

It prints, per format, the average file size, the extracted text size, the time for one thread and for N threads, MiB/s of file data, and files/s, then the peak working set of the whole run. Results of one run on the reference machine are in `benchmarks/results/extract_by_format.txt`; they are single runs on synthetic text (deflate-free generated files for PDF), so read them as relative costs between formats, not as promises. The PDF generator writes uncompressed pages; real PDFs add the cost of inflating their streams.
