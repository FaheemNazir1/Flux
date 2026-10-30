# Milestone 10 — product audit and search experience

Scope: Flux 0.9.0 on Windows (GCC 16.2 / MSYS2 UCRT64, Windows 11). Everything below was measured or tested
on that one machine; nothing was run on Windows 10, MSVC, Linux or a second computer.

The intended experience is: *open Flux, type a file name or words from inside a document, see where the file
is, open it.* Flux searches **only the folders you have registered and synchronized**. It does not scan the
computer by itself and cannot find a file in a folder it has not been given.

## 1. Gap analysis (state before Milestone 10, and now)

| Capability | Before | Now | Evidence |
|---|---|---|---|
| Search document contents | Yes, 12 text types, all words must occur | Unchanged | `test_search`, `test_sync` |
| Search file names | **No.** Only the 12 text types were in the index at all, and only by content | **Yes, any file type** in a synchronized folder (recorded by name, never opened) | `testFileNamesOfOtherTypesAreIndexedWithoutBeingRead`, `testFilenameSearch*`, GUI smoke |
| Search all registered roots by default | Yes (`--root` / "Search in" narrows) | Unchanged | `testFullWorkflowAcrossTwoRootsWithTheDefaultDatabase`, GUI scope checks |
| Removing one root leaves the others intact | Yes | Unchanged, now also for name-only records | `testRemovingOneRootLeavesTheOtherRootsNamesAndWords` |
| Partial file name | No | Yes (`tax_ret`, `holiday ph`) | `testFilenameSearchExactPartialAndWithoutExtension` |
| File name without extension | No | Yes (`notes` finds `notes.md`) | same |
| Several words | Content only (AND) | Name (every word in the name) or content (every word in the text) | `testMultipleWordsMustAllBeInTheName` |
| Case-insensitive | Yes for ASCII letters | Same; also for names | `testFilenameSearchIsCaseInsensitiveAndIgnoresPunctuation` |
| Punctuation / extensions | Punctuation separates words; `a.txt` is the words `a`, `txt` | Names: `tax-return` matches `tax_return`, extension is part of the name; a query of only punctuation is still an error | same, `testPunctuationOnlyQueryIsStillAnError` |
| Open a result in its application / in Explorer | Yes (scripts open in an editor, never run) | Unchanged; plus right-click menu and Menu key | GUI smoke (handlers), by hand not automated |
| Full path visible | Folder in a column; full path only on open | Full path in the status line on selection, and **Copy full path** (menu, `Ctrl+C`) | GUI smoke (clipboard) |
| Type, size, date, match kind, snippet | Score and path only | All shown; size and date are the values recorded at indexing time; snippet read from the file at search time | `testMetadataAndDisplayHelpers`, `testSnippetsComeFromTheFileAsItIsNow` |
| Search while a sync runs | Yes (SQLite WAL, one connection per call) | Unchanged | `test_service`, GUI smoke |
| Deleted / renamed files | Handled by the next sync (record removed / replaced); opening a vanished file shows a message | Same, now including name-only records | `testNameOnlyFilesFollowDeletionRenameAndModification`, GUI smoke |
| Empty / no-result / searching states | Existing | Texts reworded for names; "Searching..." heading kept | GUI smoke |

### Behaviour changes (deliberate, documented)

* `flux sync` and the app's Sync now also record **file names of other types**. This is what makes file-name
  search possible. `flux sync --content-only` restores the old behaviour (and forgets the names it had recorded).
* `flux search` prints a type/size/date line and a snippet under each result, and may now list files that match
  by name. `flux search --content-only` restores the old matching and the old one-line-per-result output.
* A binary file, or a text-type file over 16 MiB, is no longer reported as a skipped-with-warning file when names
  are recorded; it is stored by name.
* The legacy `flux <dir>` and `flux index <dir>` commands are unchanged.

### Known limits that remain

* Words inside PDF, Office, image or other non-text files are not searchable, only their names.
* The tokenizer is ASCII-only: accented or CJK words inside text files are not searchable. File **names** with such
  letters work (exact bytes; case-insensitive for ASCII only).
* No prefix, phrase or fuzzy matching in content. Ranking is raw frequency.
* File-name search reads every indexed row of the database (about 0.3 ms per thousand files on this machine, see
  below), so it is linear in the number of indexed files.
* Snippets and the "Open" action reflect the file on disk *now*; size and date in the list reflect the last sync.
* Nothing watches the disk: new or moved files are found only after the next sync.

## 2. Performance audit (measured, not estimated)

Machine: AMD Ryzen 5 5600H (6 cores / 12 threads), 7.3 GB RAM, SSD, Windows 11; GCC 16.2.0 Release (`-O3`);
SQLite 3.54.1. Warm OS file cache. Data is synthetic and generated into temporary folders by `flux_bench`
(never the real database). Tool: `flux_bench sync` (see `benchmarks/README.md`); raw output in
`benchmarks/results/m10_sync_*.txt`. Single runs or medians of 3 as stated: treat differences under ~10 % as noise.

### Where the time goes

| Stage | Finding |
|---|---|
| Database writes | Dominant for text files: the first sync of 5,000 files / 2.0 M tokens takes ~6.7-8.5 s (about 600-750 files/s); of the single-threaded pipeline 72 % is SQLite writing (`benchmarks/results/final_stages.txt`). More threads barely help (one writer thread by design). A trial with foreign-key checks off cut the write stage only 18 % (1.99 s to 1.62 s); **not adopted**, because it would weaken integrity protection for a modest gain. |
| Reading + tokenizing | About 16 % + 4 % + 7 % (term frequencies) of the single-threaded time; scales with threads. |
| Directory listing | Was single-threaded and cost 25-42 us per file (one `std::filesystem` query per entry). **Optimized in this milestone** with a Windows `FindFirstFileEx` lister (same rules: links not followed, system folders skipped, errors marked unverifiable; all scanner/sync/CLI tests pass). |
| Per-file `stat` in sync | Still two system calls per file; now the main cost of an unchanged re-sync (parallel). Not changed: reusing listing data would change the stored timestamps' provenance and was judged too risky for this milestone. |
| GUI progress | Progress callbacks fire every 2,000 files and are posted to the window; no measurable cost. The listing phase shows no file count (a marquee bar only). |
| Search | See below. |

### Directory listing and sync, before and after the lister change (same machine, same session type)

| Measurement | Before | After |
|---|---|---|
| List 5,000 text files | 134.6 ms | 41.3 ms |
| List 50,000 small files, names mode | 2,490 ms | 695 ms |
| Unchanged re-sync, 5,000 text files, 12 threads | 0.337 s | 0.143-0.151 s |
| First sync, 50,000 name-only files, 12 threads | 3.86 s | 1.84 s |
| Unchanged re-sync, 50,000 name-only files, 12 threads | 3.13 s | 1.46 s |

### Larger run (20,000 text files, 12.0 M tokens, 82.6 MiB, plus 100,000 name-only files; after the change)

| Run | Threads | First sync | Unchanged re-sync |
|---|---|---|---|
| 20,000 text files, content only | 4 / 12 | 69.9 s / 44.7 s | 1.09 s / 0.77 s |
| 20,000 text files + names mode | 4 / 12 | 50.4 s / 51.2 s | 0.90 s / 0.73 s |
| 100,000 name-only files | 4 / 12 | 6.5 s / 5.9 s | 5.65 s / 4.50 s |

(One repetition each after a discarded warm-up; the 4- and 12-thread first-sync times for text files differ by more than
thread count explains, which shows the run-to-run spread of the write-bound path. Listing 100,000 files: 1.33 s with names.)

### Search latency (limit 20; old = content-only `searchIndex`, new = `searchFiles`)

| Index size | Query | Old | New |
|---|---|---|---|
| 5,001 docs | common word | 3.3 ms | 4.5 ms |
| 5,001 docs | rare word | 0.15 ms | 1.1 ms |
| 5,001 docs | file name, no match | 0.08 ms | 1.0 ms |
| 20,001 docs | common word | 29 ms | 42 ms |
| 20,001 docs | rare word | 0.6 ms | 10.7 ms |
| 20,001 docs | file name, no match | 0.14 ms | 11.6 ms |

The new search adds one pass over the documents table for names (about 0.5 ms per 1,000 indexed files here), so a
very large index (hundreds of thousands of files) will search in the order of 100 ms or more. A first version that
fetched every content match was 4-6x slower than the old search; it was rewritten to ask SQLite for only the best
`limit` matches. A name index (for example a stored lower-case name column) would remove the linear pass but needs a
schema change, which was not done here.
