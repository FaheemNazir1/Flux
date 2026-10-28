# Flux

Flux is a local search and indexing engine written in C++17. It is being built in milestones; this repository is at **Milestone 2 (multithreaded file processing)**.

## Implemented

**Milestone 1 – foundation**
- `flux` command-line tool: recursively scans a directory and reports documents and tokens processed.
- Deterministic tokenizer: lowercases text and splits on any byte that is not an ASCII letter or digit.
- Supported extensions (case-insensitive): `.txt .md .cpp .hpp .h .c .py .js .ts .html .css .json`. Edit `supportedExtensions()` in [src/scanner.cpp](src/scanner.cpp) to change the list.
- Error handling: invalid paths, unreadable files, binary files (containing NUL bytes) and files over 16 MiB are skipped with a warning instead of crashing. Empty files count as documents with zero tokens.

**Milestone 2 – multithreaded processing**
- Scanning and processing are separate phases: the directory is scanned first (single-threaded), then files are read and tokenized concurrently.
- `BoundedQueue<T>` ([include/flux/work_queue.hpp](include/flux/work_queue.hpp)): a bounded, blocking, multi-producer/multi-consumer queue built on `std::mutex` and `std::condition_variable`. `close()` is the shutdown signal: it wakes all blocked threads, makes `push` fail, and lets consumers drain remaining items.
- `WorkerPool` ([include/flux/worker_pool.hpp](include/flux/worker_pool.hpp)): RAII pool of `std::thread` workers. The destructor always closes the queue and joins, including on error paths. An exception thrown by a handler is captured and rethrown from `finish()`.
- Each worker accumulates statistics in its own `IndexStats`; they are merged after all workers have joined, so no counters are shared. The list of skipped files is sorted, so results and warnings are the same for every thread count.
- `--threads N` option (1–256). The default is the hardware concurrency reported by the OS.

## Layout

```text
include/flux/   public headers (tokenizer, scanner, indexer, work_queue, worker_pool, options)
src/            implementation + main.cpp (CLI)
tests/          test executables, registered with CTest
sample_docs/    small corpus for trying the CLI
benchmarks/     placeholder
```

## Prerequisites

- A C++17 compiler with `std::thread` support (MSVC, or MinGW-w64/MSYS2 g++)
- CMake 3.16 or newer

## Build (Windows, PowerShell)

```powershell
cmake -S . -B build
cmake --build build --config Release
```

With MSYS2 UCRT64 and MinGW Makefiles:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

## Run

```text
flux [--threads N] <directory>

  --threads N   number of worker threads, 1-256 (default: hardware concurrency)
  -h, --help    show usage
```

The executable is `build\flux.exe` with single-config generators (MinGW Makefiles, Ninja) and `build\Release\flux.exe` with Visual Studio.

```powershell
.\build\flux.exe sample_docs
.\build\flux.exe --threads 4 sample_docs
```

Example output (the `Threads` value depends on the option or machine):

```text
Flux scan complete
  Threads:             4
  Documents processed: 4
  Empty documents:     1
  Tokens processed:    33
  Unsupported files:   1
  Skipped (errors):    0
```

Exit codes: `0` success, `1` invalid directory or worker failure, `2` bad usage (including an invalid `--threads` value).

## Test

```powershell
ctest --test-dir build --output-on-failure              # single-config generators
ctest --test-dir build -C Release --output-on-failure   # Visual Studio
```

Tests cover the tokenizer, scanner, queue (FIFO, close/drain, blocked producers and consumers released by `close()`, multi-producer/multi-consumer delivery), worker pool (exactly-once execution, join on destruction, handler exceptions), option parsing, and that the indexer yields identical statistics for different thread counts. None of the tests depend on sleeps or timing.

## Current limitations

- Nothing is persisted. Flux only counts documents and tokens.
- No search and no term index yet.
- Scanning is single-threaded; only reading and tokenizing run in parallel.
- Tokenizer is ASCII-only: non-ASCII bytes (e.g. accented letters) act as separators.
- Symbolic links are not followed.
- No performance measurements have been made; no speed-up is claimed.

## Planned milestones

1. Foundation (done)
2. Multithreaded producer–consumer document processing (done)
3. SQLite3 persistent index
4. Exact-term and multi-term search
5. Incremental updates (create / modify / delete)
6. Linux filesystem monitoring with `inotify`
7. Benchmarks and further tests
