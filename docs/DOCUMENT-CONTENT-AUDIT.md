# Document content search — audit, design and results

Scope: Flux 0.9.0 (version unchanged) on Windows 11, GCC 16.2 / MSYS2 UCRT64, one machine. Every number below was measured on that machine; nothing was run on Windows 10, MSVC or a second computer. The user's real database and documents were only ever read: all re-indexing was done on **copies** of the database in a scratch folder.

## 1. Why searching "Faheem" on the C: drive returned only a few files

The index of C: was complete — the disk held 74,941 files, the index 74,913 (the difference: files that appeared during the audit and a folder Windows would not list). The sync had finished (a `last_sync` stamp was present), the results came from the right database, and default exclusions (`Windows`, `Program Files`, `ProgramData`, `AppData`, ...) were applied as documented. The real causes were three:

1. **No reader for documents.** Flux could read the *text* of exactly 12 file types. A PDF, Word or PowerPoint file was recorded by name only: of 96 PDF, 59 DOCX and 21 PPTX files on C:, **0** had any searchable content. That alone hides most of the places a name appears.
2. **The list of readable types was too short even for plain text.** `.log` (482 files), `.xml` (342), `.csv` (65), `.ini` (103), `.toml` (271), `.yaml/.yml`, `.sql`, `.java`, `.tsx/.jsx`, ... were name-only.
3. **`D:\` was not indexed at all** (601,287 files, among them 512 PDFs, 27,384 `.md` and 5,570 `.txt` files); only `D:\Project\Flux\sample_docs` was registered. Searching can only find what was registered.

What the audit of the old index showed for "faheem": 22 files with the word in their *content* (all plain text) and 29 files with it in their *name* — which is the "few documents" that appeared.

### Counts by extension (old index, 74,913 files: 18,857 with searchable words)

| Extension | Files in index | With searchable content (before) |
|---|---:|---:|
| (none) | 14,022 | 0 |
| .jpg | 12,054 | 0 |
| .pyi | 11,161 | 0 |
| .js / .py / .json / .h / .ts | 5,445 / 4,040 / 2,985 / 2,589 / 1,046 | ~99 % |
| .md / .html / .txt | 933 / 851 / 447 | ~99 % |
| .log | 482 | 0 |
| .xml | 342 | 0 |
| .pdf | 96 | **0** |
| .docx | 59 | **0** |
| .pptx / .ppt | 21 / 1 | **0** |
| .csv | 65 | 0 |
| .xlsx / .xls / .doc / .odt / .rtf | 0 | – |

## 2. What was built

Flux now reads the text of PDF, DOCX, PPTX, XLSX, ODT/ODS/ODP, RTF, HTML, XML, EML, EPUB, ZIP (entries that are themselves supported documents) and ~60 text/source/data extensions (UTF-8, UTF-16 and Windows-1252 text). All readers are Flux's own code (DEFLATE, ZIP, PDF parser and content-stream interpreter, XML scanner): **no third-party library was added**, so there is no new licence, redistribution duty or package-size cost.

- **Safety.** Nothing is executed; macros, scripts, JavaScript in PDFs, forms, embedded objects and external references are never opened. Each file has a size and a time budget; decompression output and ZIP entries are capped; a damaged, hostile or encrypted file yields a *status*, never a crash or a failed sync.
- **Locations.** Search shows `page 15`, `slide 3`, `slide 3 notes`, `Students!B7`, `sheet Budget`, `section 2` or the entry name inside an archive, found by reading the document again when the snippet is built (the index stores no text and no schema change was needed for this).
- **Status per file.** A new table `content_status(doc_id, status, extractor, detail)` records *why* a file is or is not searchable; see section 4.

### Supported and unsupported formats

| Supported (content) | Notes |
|---|---|
| PDF | all pages, compressed/object/xref streams, ToUnicode maps, simple and composite fonts, Type 3 fonts, form XObjects; damaged cross-reference tables are rebuilt; info dictionary |
| DOCX/DOCM/DOTX | paragraphs, headings, tables, headers/footers, footnotes/endnotes/comments, core properties, page numbers where Word recorded page breaks |
| PPTX/PPSX | slide text and tables in presentation order, speaker notes |
| XLSX/XLSM | sheets, shared/inline strings, numbers, cached formula results (formula text when none) with `Sheet!Cell` |
| ODT/ODS/ODP, RTF, HTML, XML, EML, EPUB, ZIP | see README |
| text and source | .txt .md .log .csv .tsv .json .yaml .toml .ini .sql .c .cpp .h .cs .java .py .js .ts .tsx .jsx .css .sh .bat .ps1 .rb .php .go .rs ... |

| Not supported | Why |
|---|---|
| .doc / .xls / .ppt | Old binary OLE formats. A safe reader needs a compound-file parser plus the Word piece table / Excel BIFF / PowerPoint record parsers (several hundred lines each, untested against real files here). Not done; they stay searchable by name. |
| .msg | Outlook OLE container: same parser needed. |
| .mobi | Palm DOC compression and often DRM. |
| .7z .rar .gz .tar | No decoder written. |
| Images (.jpg .png ...) and scanned PDFs | Need OCR — see section 5. |
| Password-protected files | By design; see section 3. |

## 3. Password-protected files and Windows permissions

- Password-protected PDFs (`/Encrypt`) and Office/OpenDocument files (OLE `EncryptedPackage` container, ODF `encryption-data`) are detected; their *name* stays searchable and they are reported as **"Password required"**. Flux never tries to guess, crack or bypass a password and stores none. None of the 608 PDFs on this machine were encrypted, so that path is covered by test fixtures only. A user-supplied-password feature was *investigated, not built*: it would need PDF decryption (RC4/AES with MD5/SHA-2) and Office agile encryption, and a secure store (Windows DPAPI `CryptProtectData` would keep a password readable only by the same Windows user). It needs your approval before any password is stored.
- Windows permissions are never bypassed and administrator rights are never requested. A file that cannot be opened is **access denied**: if it already had content, that content is kept and the file is tried again at the next sync; a folder that cannot be listed is reported separately (`Folders access denied`) and **never treated as empty** — what is indexed under it is kept. This is tested with a real `icacls` deny permission on a temporary folder, with a file held open by another handle, and with a simulated scan failure.
- Default exclusions (`Windows`, `Program Files`, `ProgramData`, `Recovery`, `$Recycle.Bin`, `System Volume Information`, `AppData`, ...) are unchanged. The sync summary now lists the skipped folders **with the reason** ("system or application folder" / "excluded by you"), and exclusions remain configurable (Settings > Folders to skip, `--exclude`).

## 4. Database change and the re-indexing procedure

Schema version stays 2 (older Flux builds keep working: they ignore the new table, and deleting a document cascades to its status row). Opening an older database **for writing** adds `content_status` after making a complete copy (`<name>.pre-content.bak`, never overwriting a copy, refused if the disk is too full), in one transaction; nothing existing is altered. Reading never changes it.

For the existing C: index:

```
flux sync C:/        # adds the table (after the backup), then reads every file that has a reader but no content yet
flux content         # how much is searchable, and why the rest is not   (--by-extension, --list password|denied|corrupted|...)
flux reindex --content --all     # later: retry password-protected / damaged / unreadable files
```

A normal sync reads new and changed files, files that were recorded by name only, and files read by an older version of a reader (each kind has a reader version stored with the status); it skips everything that is up to date. `--force` re-reads everything that has a reader. Plain-text types indexed before statuses existed are kept as valid, so they are not read again.

## 5. OCR (investigated, not implemented)

Searching inside scans and pictures needs OCR. Nothing was installed and nothing is claimed: scanned PDFs are reported as **"image only (needs OCR)"** (7 on C:) and pictures as unsupported.

| Option | Licence | Install / size | Cost | Notes |
|---|---|---|---|---|
| Windows.Media.Ocr (built into Windows 10/11) | part of Windows; nothing to redistribute | nothing to install; languages depend on installed language packs | modest CPU, no extra memory in Flux's process | WinRT API: most practical for a Windows-only app; reachable from MinGW through a small helper process. Page images of a scanned PDF would still have to be extracted (JPEG streams can be passed straight through; other image codecs need decoders). |
| Tesseract | Apache-2.0 (Leptonica BSD-2) | engine + language data: roughly 10-40 MB added to the package | about 0.5-3 s per page per core, 100-400 MB RAM | Must ship licence notices; best accuracy offline. |
| ONNX-based engines (e.g. PaddleOCR) | Apache-2.0 | 20 MB+ models plus a runtime | highest CPU/RAM | Heaviest. |

Recommendation: an optional OCR step using Windows.Media.Ocr, off by default, run only on files already marked "image only", with a page limit and the same pause/cancel controls. Needs your decision.

## 6. Results

### Real data — C: drive (copy of the database; your files read-only)

| | Before | After |
|---|---:|---:|
| Files in the index | 74,913 | 75,014 |
| Searchable by content | 18,006 | **32,952** |
| Password required / access denied / corrupted | – | 0 / 4 / 5 |
| Image only (scans) | – | 7 |
| PDF files with content | 0 of 96 | 88 of 99 (7 scans; 2 are 0-byte files; 1 has no text, as `pdftotext` also finds; 1 is not in the first-index count) |
| DOCX with content | 0 of 59 | **59 of 59** |
| PPTX with content | 0 of 21 | 20 of 21 (1 is an Office lock file `~$...pptx`) |
| ZIP with content | 0 of 10 | 9 of 10 |
| Search "faheem" | 22 content + 29 name matches | **162 results** (PDF, DOCX, PPTX, text, JSON, logs, ...) |

The re-index (15,664 files read, 108 added) took 2 min 02 s with 12 threads. The unreadable items were: `C:\Users\WsiAccount` (a folder Windows refuses to list; its entries were kept), 5 files that were in use or access denied (a system log and four small `hp` files), and 4 system files that vanished while the scan ran (`hiberfil.sys`, `pagefile.sys`, `swapfile.sys`, `DumpStack.log.tmp`).

### Extraction accuracy

- Against `pdftotext` (reference tool, used only for this comparison): **98.4 %** of its words found by Flux's reader over 83 real PDFs. One outlier (a file whose *name* the comparison script could not pass on the command line) counts as 0 %. A real bug found this way — browser-made PDFs with Type 3 fonts came out as "G o v e rn m e n t" — was fixed and has a regression test.
- Known limits: PDF fonts without a character map (some symbol/CID fonts) can come out empty; layout order of multi-column PDFs follows content-stream order.

### Tests (exact counts)

- Debug-style tree (`build/`, no optimisation): **25 of 25** CTest programs pass. Release tree (`build-release/`, -O3): **25 of 25**.
- New: `test_extract` (14 tests) reads every format from real files each containing a unique word and checks text, page/slide/sheet labels, passwords, corrupted/truncated/empty/scanned files, limits and cancellation; `test_content` (16 tests) runs them through the real sync into a temporary database and searches: unique words and locations for 32 expectations, "faheem" across kinds, statistics, incremental sync, modification and deletion, re-extraction of old records, reader-version upgrade, `reindex`, a file held by another program, a real Windows ACL deny, simulated denied folders, skipped-folder reasons, migration with backup, cancellation, several roots, thread-count independence, large documents; `test_cli` (+1 test) covers `content`, `reindex`, the sync summary and search output. GUI self-test: **109 checks, 0 failed** (Debug and Release trees).

### Performance and memory (single machine, Release)

Generated documents, 24 files per format, ~2 MiB of text each (`benchmarks/results/extract_by_format.txt`):

| Format | 1 thread, MiB/s of file | files/s | 4 threads (total) |
|---|---:|---:|---:|
| txt | 57.7 | 32.2 | 0.26 s |
| docx | 54.1 | 22.5 | 0.31 s |
| rtf | 44.1 | 23.6 | 0.28 s |
| csv | 38.7 | 19.3 | 0.35 s |
| xlsx | 35.2 | 9.5 | 0.69 s |
| html | 30.5 | 14.4 | 0.39 s |
| pptx | 23.2 | 7.6 | 0.86 s |
| pdf (uncompressed) | 11.0 | 5.3 | 1.15 s |

Peak memory: the whole benchmark run peaked at 34 MiB; reading a 9 MiB Word file plus a 4,000-page PDF with 4 workers grew the test process by 49 MiB; an 18 MB, 22-page vector-heavy real PDF takes about 1.2 s.

## 7. Remaining limitations

- No OCR; no `.doc/.xls/.ppt/.msg/.mobi`; no phrase search (quoted queries are not special) and ranking is still by word frequency; the tokenizer is ASCII-only (accented or CJK words inside documents cannot be searched, though the text is extracted correctly).
- Snippets re-read the document when a search is shown (bounded to 8 s per file); a result list full of large PDFs is slower than one full of text files.
- Whole-drive sync is still one transaction per root: cancelling a very large first sync discards its progress (not changed in this work).
- The PDF reader is new code checked against 83 real PDFs and the fixtures; the other formats were checked against fixtures and the files present on this machine (no XLSX was available on C:, one on D:).
- Installer (Inno Setup) still never compiled; package unsigned; nothing committed, pushed or published.
