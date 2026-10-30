# Flux

Flux is a local search and indexing engine written in C++17. You choose folders (or drives); Flux records the names of their files and reads their text files into a SQLite index, and then finds files by part of their **name** or by the **words inside** them, across every folder you chose, without re-scanning anything. Forgotten where you saved `Tax Return 2023.pdf`? Type `tax 2023`.

This repository is at **document content search** (words inside PDF, Word, PowerPoint, Excel and other documents), after Milestone 11 (drive discovery) and Milestone 10 (search by name and content), on top of the Windows desktop app, multi-root indexing and packaging. Flux searches **only what you have registered and indexed**. Files of **any type** can be found by their name; words *inside* files are searchable for the types Flux can read: text and source code, HTML, XML, RTF, **PDF, Word (`.docx`), PowerPoint (`.pptx`), Excel (`.xlsx`)**, OpenDocument (`.odt .ods .odp`), e-mail (`.eml`), e-books (`.epub`) and the supported documents inside ZIP archives; see [Searching inside documents](#searching-inside-documents). It does not search the whole computer unless you register every drive and folder you care about, and even then it skips operating-system and application folders by default (see [Whole-PC use](#whole-pc-use)). See [Searching: names and contents](#searching-names-and-contents).

## Quick start (Windows, PowerShell or cmd)

Build once, then use the launcher from any folder:

```powershell
.\build-flux.bat                                   # one-time build (needs CMake and MSYS2 UCRT64 g++)

.\run-flux.bat root add "C:\Users\<you>\Documents" # 1. choose a folder to index
.\run-flux.bat root add "D:\Projects"              #    ...and as many more as you like
.\run-flux.bat sync --all                          # 2. read their files into the index
.\run-flux.bat search invoice 2024                 # 3. search all of them at once
.\run-flux.bat sync --all                          # later: bring the index up to date
.\run-flux.bat status                              # what is indexed, and is every folder available?
```

Prefer a window? `.\run-flux-gui.bat` opens the [desktop app](#desktop-app) on the same index: the same folders, the same database, the same search.

`<you>` and the paths are placeholders for your own folders. The index is stored in `%LOCALAPPDATA%\Flux\flux.db` (see [Where the database lives](#where-the-database-lives)), so you never need to say where it is. Add the repository folder to your `PATH`, or copy `run-flux.bat` shortcuts wherever you like, if you want to type just `run-flux`; the launcher never changes `PATH` itself.

`run-flux.bat` finds the program next to itself (`build-release\flux.exe`, else `build\flux.exe`), runs it from whatever folder you are in, passes all arguments through, and returns its exit code. If Flux has not been built it says so and tells you how; it never builds or downloads anything on its own. Set `FLUX_EXE` to the full path of a `flux.exe` to use a different one.

**Quoting.** Put paths in double quotes, always if they contain spaces and also if they contain `& ^ | < > ( ) %`, because `cmd.exe` interprets those inside a `.bat`. Do not end a quoted path with a backslash (`"C:\My Docs\"` makes Windows treat `\"` as a literal quote); write `"C:\My Docs"`.

## Commands

| Command | What it does |
|---|---|
| `flux root add <dir>` | Register a folder or drive as a root. Nothing is read yet. Registering it again does nothing. |
| `flux root list` | Registered roots, files indexed in each, and whether each is currently available. |
| `flux root remove <dir>` | Unregister a root and delete **its** entries from the index. Other roots and your files are untouched. Works for a root on a disconnected drive. |
| `flux sync <dir>` | Make the index match one root: new files indexed (the names of files of other types are recorded too, never opened), changed files re-indexed, unchanged files skipped, deleted files removed. `--content-only` records only the text files, as before Milestone 10. |
| `flux sync --all` | The same for every registered root that is currently available (others are skipped with a warning). |
| `flux index <dir>` | Read the files under `<dir>` (inside a registered root) into the index, replacing their entries; removes nothing. |
| `flux search <words...>` | Files whose **name** contains all the words, or whose **content** does, best match first, with the full path, type, size, date and a snippet. `--root <dir>` limits it to one root; `--limit N` caps the results (default 20, maximum 10000); `--content-only` searches file contents only (the behaviour before Milestone 10). |
| `flux drives` | List this computer's drives (letter, label, type, file system, capacity, free space) and which are indexed: the whole drive, some folders, or nothing; plus registered folders on drives that are not connected. Read-only; nothing is indexed automatically. |
| `flux content` | How much of the index is searchable by **content**, and why the rest is not: extracted, awaiting extraction, unsupported formats, password required, access denied, corrupted, scans without text, ... `--by-extension` groups the counts by file extension; `--list password` (or `denied`, `corrupted`, `awaiting`, `unsupported`, `imageonly`, `notext`, `toolarge`, `binary`) lists the files with the reason. Read-only. |
| `flux reindex --content` | A sync that also reads again the files whose content could not be extracted before (password removed, file repaired, ...). `--force` reads every file that has a reader again. Never deletes anything. |
| `flux status` | Database location and size, schema version, number of indexed files, and the roots. |
| `flux help [command]` | Help. Every command also accepts `--help`. |

Common options: `--db FILE` (use this database instead of the default one), `--threads N` (worker threads, 1–256, default = your CPU's hardware threads; `sync` and `index`), `--verbose` (show every warning, not just the first 10), `--include-system` (also scan the system and application folders listed under [Whole-PC use](#whole-pc-use)), `--exclude DIRS` (`sync` only: skip these folders and everything below them; separate several with `;`).

A folder whose name is exactly `drives` must also be written as `.\drives`.

Exit codes: `0` success, `1` runtime error (folder not found, drive not available, nothing indexed yet, database problem, directory not a registered root), `2` usage error (unknown command or option, missing argument). Warnings do not change the exit code.

A folder whose name is exactly `root`, `sync`, `index`, `search`, `status` or `help` must be written as `.\sync` etc.

**Older commands still work** exactly as before:

```text
flux [--threads N] [--db FILE] <directory>     count files and tokens; stores them only with --db (never in the default database)
flux sync --db FILE <directory>                 (--db is now optional)
flux search --db FILE <terms...>                (--db is now optional)
```

### Example session

```text
> flux root add "C:\Data\Documents"
Registered root: C:/Data/Documents
Next: flux sync "C:\Data\Documents"   (reads the files into the index)

> flux sync --all
C:/Data/Documents: 2 added, 0 updated, 0 removed, 0 unchanged
C:/Data/Work Projects: 1 added, 0 updated, 0 removed, 0 unchanged
Synced 2 roots   Database: C:\Users\<you>\AppData\Local\Flux\flux.db

> flux search acme
3 result(s):
  1  C:/Data/Documents/invoice.txt
      TXT file, 26 bytes, modified 2026-10-29 14:03
      invoice for acme total 100
  1  C:/Data/Documents/notes/todo.md
      MD file, 20 bytes, modified 2026-10-29 14:03
      pay the acme invoice
  1  C:/Data/Work Projects/readme.md
      MD file, 20 bytes, modified 2026-10-29 14:05
      acme website project

> flux search holiday photo
1 result(s):
  name  C:/Data/Documents/Holiday Photo 2024.jpg
      JPG file, 2.1 MiB, modified 2026-08-02 09:41

> flux status
Flux status
  Database:   C:\Users\<you>\AppData\Local\Flux\flux.db (default location, 48.0 KiB)
  Schema:     2
  Files:      3 indexed, 10 distinct words
  Roots:
    C:/Data/Documents   2 files   available
    C:/Data/Work Projects   1 file   available
```

Each result is `match  path` followed by the file type, size and modification date **as recorded when the file was last indexed**, and, for a content match, a snippet. `match` is `name` for a file-name match, a number (the total occurrences of the words in the file) for a content match, and `name+3` for both. Paths are shown with forward slashes in a fixed, normalized form.

## Searching inside documents

A search for a word finds it **inside** these kinds of file, as well as in file names:

| Kind | Extensions | What is read | Location shown with a result |
|---|---|---|---|
| PDF | `.pdf` | text of every page (compressed streams, object streams, cross-reference streams, ToUnicode maps; a damaged cross-reference table is rebuilt), document title/author/subject/keywords | `page 15` |
| Word | `.docx .docm .dotx` | paragraphs, headings, table cells, headers, footers, footnotes, endnotes, comments, document properties | `page 2` (where Word recorded page breaks) |
| PowerPoint | `.pptx .pptm .ppsx` | slide text, tables, **speaker notes**, in presentation order | `slide 3`, `slide 3 notes` |
| Excel | `.xlsx .xlsm` | sheet names, cell values (text and numbers), cached formula results (a formula with no cached result keeps its formula) | `Students!B7` |
| OpenDocument | `.odt .ods .odp` | text, tables, slides, sheets, properties | `slide 2`, `sheet Budget` |
| RTF, HTML, XML | `.rtf .html .htm .xhtml .xml ...` | text only (scripts, styles, comments, font tables and markup are dropped) | |
| E-mail, e-book | `.eml`, `.epub` | headers, text and HTML bodies (quoted-printable/base64 decoded), attachment names and text attachments; chapters | `section 2` |
| ZIP archive | `.zip` | the names of the entries, and the content of every entry that is itself a supported document (two levels deep) | the entry name |
| Text, source, data | `.txt .md .log .csv .json .yaml .toml .ini .sql .py .js .ts .tsx .jsx .java .c .cpp .h .cs .go .rs ...` | UTF-8, UTF-16 and Windows-1252/Latin-1 text | |

**Nothing is ever executed.** Macros, scripts, forms, embedded objects, JavaScript in PDFs and external links are never run or followed. Files are read through bounded buffers (a 4,000-page PDF and a 9 MB Word file grew the process by about 50 MiB in the tests), each file has a time and size budget, and a damaged, hostile or password-protected file produces a *status*, never a crash.

**Why a document may not be searchable by content** (`flux content` and **View > Content status** show the counts; selecting such a result in the app, or `flux search`, says why):

- **Password required**: a password-protected PDF or Office/OpenDocument file. Its name and anything readable without the password stay searchable. Flux never tries to guess or bypass a password and does not store passwords.
- **Access denied**: Windows did not let Flux open the file (permissions, or another program holds it). Any content already indexed is kept, and the file is tried again at the next sync. Folders Windows refuses to list are reported separately and are **never treated as empty**: what was indexed below them is kept.
- **Corrupted**, **no text**, **too large**, **not text** (binary data in a text file).
- **Scan without text** (a PDF of page pictures, or `.jpg/.png`): searching inside pictures needs OCR, which is **not implemented**.
- **Unsupported format**: the old binary `.doc`, `.xls`, `.ppt`, Outlook `.msg`, `.mobi`, `.7z`, `.rar`, images, audio, video and programs. They are found by name.

**Re-reading what was indexed before.** The database that existed before documents could be read needs no rebuilding. Opening it for writing adds one small table (`content_status`) after saving a copy (`<name>.pre-content.bak`); the next `flux sync` then reads the files that had been recorded by name only and every file that was read by an older version of a reader, and skips everything that is up to date. `flux reindex --content` additionally retries the failures. Search results show the page, slide, sheet or cell where the words were found.

Limits: the tokenizer is still ASCII-only (letters outside A-Z are word separators); there is no phrase search; quoted queries are not special; ranking is by how often the words occur.

## Searching: names and contents

One query looks at both the file name and the file's text:

- **By name** (any file type that is in the index). Every word of the query must occur somewhere in the file name, extension included, ignoring case: `report pdf` finds `Report_Final.PDF`; `tax 2023` finds `tax_return_2023.txt`; `tax-return` finds `tax_return.txt`; `notes` finds `notes.md` (without typing the extension); `holiday ph` finds `Holiday Photo 2024.jpg`. Words in folder names do not count. Names with spaces and non-ASCII letters work; upper/lower case is ignored for ASCII letters only.
- **By content** (every readable document type, see above), under the rules of earlier milestones: every word must occur in the text, case-insensitively, punctuation ignored.
- **Ranking:** name matches first (exact file name, then whole words, then names starting with the query, then any other substring), then by how often the words occur in the text, then by path. A file can match both.
- **All folders by default.** A search covers every registered folder; choose one folder only if you want to narrow it. Removing a folder never affects the files indexed from another folder (the same name in two folders is two results).
- **How the names get there:** `flux sync` (and the app's Sync) records the name, size and date of every file under a root. Files with no reader (images, programs, ...) are recorded **by name only** and never opened; documents that cannot be read (password required, damaged, over the size limit) are recorded by name and the reason is kept. They cost one small database row each. `flux sync --content-only` skips this and also forgets the names it had recorded.
- **Snippets** are read from the file when you search (the index keeps no text), so they show the file as it is now; PDF, Office and other documents are read with the same readers that indexed them, which is how the page, slide, sheet or cell is known. A file that has gone or changed since is simply shown without one.
- **Only indexed folders.** A file in a folder you have not added, or created since the last sync, cannot be found. The search never scans the disk.

## Desktop app

`flux-gui.exe` is a native Windows window over the same engine and the same database as the command line. Anything you do in one is visible in the other, and the command-line tool works whether or not the app is open.

```powershell
.\build-flux.bat          # builds both flux.exe and flux-gui.exe (one time)
.\run-flux-gui.bat        # opens the app and returns immediately
```

**What you get**

- A search box with a "Search in" choice (all folders, or one). Results show how each file matched (`name`, a content score, or `name + 3`), the file name, type, size, modified date and folder; selecting a result shows its full path and a snippet of the matching text in the status line. Every word must match, in the name or in the text (see [Searching: names and contents](#searching-names-and-contents)). Double-click or press Enter to open a result; right-click (or the Menu key) for **Open**, **Open containing folder** and **Copy full path** (`Ctrl+C` also copies it); the buttons below the list do the first two.
- A **Folders** panel: *Add folder...* (the standard Windows folder picker), *Remove* (asks first; deletes only that folder's index entries, never your files), *Sync selected* and *Sync all*. A folder on a disconnected drive is shown in red as "Not available"; selecting a folder shows its full path in the status line.
- A dashboard line (files indexed, distinct words, folders, database size and location), a status line with a progress bar and a **Cancel** button, and a log of what was done and any warnings.
- File > Settings: the database file (empty = the default location), worker threads (0 = automatic), whether to also scan system and application folders, and how many results to show. Settings are stored in `%LOCALAPPDATA%\Flux\gui-settings.ini`, separately from the database.
- Clear empty states: a welcome screen before anything is added, "no index yet" before the first sync, a hint on what to type, a "Searching..." heading while a query runs, and "no indexed file contains all of these words in its name or its text" when nothing matches. The window stays responsive while searching and while syncing.

- A **Drives** panel (see [Drives, whole-drive indexing and exclusions](#drives-whole-drive-indexing-and-exclusions)): every drive with type, free/total space and index status, multi-select, *Index selected drives*, *Refresh drives*; and **Pause/Resume** beside Cancel while a sync runs.

**Keyboard:** `Ctrl+F` search box, `Enter` search (in the results list: open the file), `Ctrl+C` in the results copies the full path, `Ctrl+O` add a folder, `Ctrl+S` sync the selected folder, `Ctrl+Shift+S` sync all, `Delete` remove the selected folder (asks first), `F5` refresh, `Tab` / `Shift+Tab` move between controls. Help > Keyboard shortcuts lists them.

**Typical first use:** *Add folder...*, choose a folder, answer "Index it now?" Yes. Type a word, press Enter.

**Safe by design**

- Slow work (sync, searches, loading the dashboard) never runs on the window's thread, so the window stays responsive. Only one writing operation runs at a time: while one is running, Add / Remove / Sync buttons are disabled, so the same folder can never be synchronized twice at once. Searching still works during a sync (it sees the last completed state).
- **Cancel**, or closing the window during a sync, stops it and leaves the index exactly as it was: a sync is one all-or-nothing transaction. Closing the window waits for the running operation to stop before the window goes away.
- A failed operation is reported as a failure (status line and log), never as a success. A folder that cannot be reached keeps its index entries.
- **Opening a result never runs it.** Scripts (`.js`, `.py`) are opened in an editor instead of being executed, because a double-click on them would otherwise run them. Other files open in their normal application; if Windows has none, in Notepad. File paths are handed to Windows as paths, never spliced into a command line.
- The same overlap and canonical-path rules as the CLI apply (the app calls the same functions).

**How it is built.** Plain Win32 and Common Controls (themed lists, buttons and progress bar, per-monitor DPI), with no new dependencies: it needs nothing beyond the compiler you already use, and Windows 10 (version 1607) or newer for the per-monitor DPI APIs. The window code lives in `gui/`; everything it asks of the engine goes through `FluxService` (`include/flux/service.hpp`), which calls the existing engine functions with one short-lived database connection per operation, and a small `JobRunner` runs background jobs. Results return to the window as posted messages, so no control is touched from another thread. Build without it with `-DFLUX_BUILD_GUI=OFF`.

**How it is tested.** The GUI-independent parts (service, job runner, settings) have unit tests, including cancellation at three different moments, a sync running while searches continue, and a job runner stress test. `test_gui_smoke` runs `flux-gui.exe --smoke-test`: the real window is created off-screen and driven through the code paths its buttons use, while the test reads back what the real list controls show. It covers search (AND semantics, ranking, no results, scope), adding and removing folders (isolation), an unavailable folder, refusal of a second operation, search during a sync, cancel, tab order and `Ctrl+F`, the settings dialog, a responsiveness probe (the window must answer within a second during a 3,000-file sync; measured worst case about 40 ms in an unoptimized build), and closing the window in the middle of a sync followed by a database integrity check. The test redirects the database and settings into a temporary folder and **refuses to run** unless it has first verified that the redirect works. Pass `-Screenshots <folder>` to `tests\test_gui_smoke.ps1` to keep BMP screenshots of each state. `test_windows_launcher` also starts the app through `run-flux-gui.bat` and checks that it opens, responds and closes (a window appears briefly).

**Not automated or not verified:** the native folder-picker and file-open dialogs (they are modal system dialogs), actually opening a file in another program or Explorer, real mouse and keyboard input on a physical machine (the tests invoke the same handlers, and the screenshots were inspected by eye), screen readers, dark mode, and display scalings other than the 125% of the development machine. Windows 10 and 11 differences, MSVC, and any non-Windows system were not tested. The window cannot be resized below about 860 x 560 (scaled).
## How it works

### Roots
A **root** is a folder or drive you registered. Roots are stored in the database (`roots` table) in canonical form: symlinks and `..` resolved and, on Windows, drive letter and every folder name spelled exactly as stored on disk. So `c:\users\you\docs\`, `C:\Users\You\Docs` and `C:\Users\You\Docs\..\Docs` are the same root, and registering any of them twice does nothing.

Every indexed file belongs to the root it was found under (`documents.root_id`). That is what makes the isolation guarantee possible: **syncing or removing one root cannot touch another root's entries**. This is enforced in three independent places (the SQL that loads a root's records, a path-prefix check when deciding what is stale, and the database writer, which only deletes documents of the root it was given). Isolation tests cover the behaviour; during development each layer was also disabled on purpose to confirm that the tests fail only when all three are defeated (any one layer alone is enough to prevent cross-root deletion).

Roots may not overlap: a folder inside a registered root, or one that contains a registered root, is refused with an explanation, so a file can never belong to two roots. `root add D:\` therefore needs `D:\Projects` to be removed first, and vice versa.

`sync` and `index` only accept registered roots. The one exception, kept from Milestone 5: if the database has **no roots at all**, the first `flux sync <dir>` registers `<dir>`.

### Sync (what counts as changed)
`sync` reconciles one root with the disk. A file is skipped without being read only if the database has a record for it with the **same size and the same modification time**, and that record can be trusted: files modified less than 2 seconds before they were indexed (or after) are re-read once ("Refreshed") because a second write inside the same timestamp tick would be invisible. No content hash is computed: hashing would require reading every file on every sync, which is exactly the cost the skip avoids.

- *New file*: indexed. *Modified file*: its old entry, postings and unused words are replaced (no duplicates). *Deleted file*: its entry is removed. A rename is a delete plus an add.
- *No longer indexable* (now binary, over 16 MiB, unsupported extension, or in a folder that is now excluded): its entry is removed, so stale content cannot stay searchable.
- Everything is applied in **one transaction**: if anything fails, the index is exactly as before; `flux search` never sees a half-synced state.
- A record is deleted only when the disk **positively shows** the file is gone or ineligible. A folder or file that cannot be checked (access denied, locked, changed while being read) keeps its old entry and is reported as a warning; it is never treated as deleted.
- A root that is not available at all (disconnected drive, deleted folder) fails the sync **before anything is changed**.

Limits of this change detection: a file rewritten with the same size and the same modification time is not noticed (for example by a tool that preserves timestamps); a file whose time changes without its content changing is simply re-indexed; the 2-second window assumes timestamp granularity of about 2 seconds or better, which was not tested on FAT or network volumes.

### Where the database lives
Default: `%LOCALAPPDATA%\Flux\flux.db` (for example `C:\Users\<you>\AppData\Local\Flux\flux.db`), created on first use. If `LOCALAPPDATA` is not set, `%USERPROFILE%\AppData\Local\Flux\flux.db` is used. It never depends on the folder you run Flux from. Override it, in order of precedence, with `--db FILE` or the `FLUX_DB` environment variable. A mistyped `--db` folder is an error, not a new folder; only the default folder is created automatically. Read-only commands (`search`, `status`, `root list`) never create a database: with none yet they print how to get started.

**Back it up** by copying `flux.db` while Flux is not running (if a `flux.db-wal` file exists next to it, copy that too). To start over, delete `flux.db` (and `-wal`/`-shm`); your files are never touched. The index can always be rebuilt with `sync --all`.

### Schema (version 2) and upgrading an older database
```text
meta      (key PK, value)                 schema_version = 2
roots     (id PK, path UNIQUE, added_at)
documents (id PK, path UNIQUE, size_bytes, mtime_ns, token_count, indexed_at,
           root_id -> roots ON DELETE CASCADE, NULL = no root)      + index on root_id
terms     (id PK, term UNIQUE)
content_status (doc_id PK -> documents ON DELETE CASCADE, status, extractor, detail)   why/whether a file's content is searchable
postings  (term_id -> terms, doc_id -> documents ON DELETE CASCADE, frequency;
           PK(term_id, doc_id), WITHOUT ROWID)                      + index on postings(doc_id)
```
Databases created by Milestones 3–5 are schema 1. **Reading** them (`search`, `status`) works and changes nothing. A command that **writes** to one (`root add`, `sync`, `index`, ...) upgrades it, and only after taking a consistent copy with SQLite's backup API to `<name>.v1.bak` next to it (an existing backup is never overwritten; `.v1.bak.1`, `.v1.bak.2`... are used instead). If the copy cannot be made, the database is left untouched and the command fails. The upgrade itself is one transaction: the single directory Milestone 5 had bound the database to becomes the first root and its files are assigned to it; files that belong to no known root stay *unassigned* — still searchable, never touched by any sync, and adopted automatically if you later register a root that contains them. Foreign keys stay enforced. A schema-2 database is refused by older Flux builds, which is the safe direction.

### Implemented so far
**M1** recursive scanner, tokenizer, basic CLI. **M2** multithreaded processing with a bounded queue and worker pool. **M3** SQLite index with a dedicated writer thread, AND-ranked search. **M4** benchmark suite and writer optimizations ([benchmarks/README.md](benchmarks/README.md)). **M5** incremental `sync`. **M6–7** root registry, default database, unified search, launcher, beginner-friendly CLI. **M8** native Windows desktop app (`flux-gui.exe`), cancellable sync, background jobs.

## Drives, whole-drive indexing and exclusions

**Discovery.** `flux drives` and the app's **Drives** panel ask Windows for the drives that exist right now: fixed disks, USB and other removable drives, optical drives and network drives, each with its label, type, file system, capacity and free space (the space you can actually use). Discovery only reads; it needs no administrator rights, writes nothing to the drive or the index, and never indexes anything by itself. A network drive or card reader that does not answer within 3 seconds is shown as "not ready" instead of freezing the window; an empty card reader shows "no disc or card in the drive".

**Indexing a drive.** Select one or more drives and click **Index selected drives** (or run `flux root add D:\` then `flux sync D:\`). Flux asks first, registers the drive as a root, and synchronizes it; several drives are processed one after the other, each under its own root, so one search covers all of them. You can **Pause** and **Resume** (nothing is committed while paused; **Cancel** still works and leaves the index as it was) and the drive's *Index* column shows *Indexing...* / *Paused*. Selecting a drive shows its details and last successful sync. A drive is "Whole drive" when its root (`D:/`) is registered and "Some folders" when only folders of it are; the two cannot be mixed (roots never overlap), so to index a whole drive remove its individual folders first. Folders you added by hand keep working exactly as before.

**Removing a drive** from the index: select its row in **Folders** and click Remove. Only Flux's index entries go; nothing on the drive is touched. **A drive that is unplugged is never forgotten:** its root stays registered, is listed as not available, is skipped by Sync all, and its entries stay searchable until you remove it.

**What is skipped by default** (unless *Also scan system and application folders* / `--include-system`): `AppData`, `$Recycle.Bin` and `System Volume Information` anywhere; and directly under a drive root `Windows` (which holds the component store), `Program Files`, `Program Files (x86)`, `ProgramData`, `Recovery`, `PerfLogs`, `MSOCache`, `Config.Msi` and the Windows setup folders `$WINDOWS.~BT`, `$WINDOWS.~WS`, `$WinREAgent`, `$SysReset`, `$GetCurrent`. Your own folders (Desktop, Documents, Downloads, Pictures, Videos, Music, projects, ...) are never skipped by default. Directory symlinks and junctions are never followed. A folder Windows will not let you read is reported as skipped (and the files that may be in it are kept in the index, not treated as deleted); Flux never asks for administrator rights or tries to get around permissions.

**Your own exclusions.** File > Settings > *Folders to skip* (one full path per line) or `flux sync --exclude "D:\Games;D:\Temp"`. Everything below a listed folder is ignored by every sync, and files indexed there earlier are removed from the index at the next sync (lift the exclusion and sync again to bring them back). An exclusion that is the synced root itself, or contains it, is ignored. The list is kept with the app settings, not in the database.

**Last sync.** Each successful sync stores its time in the database (`meta` table, key `last_sync:<root>`, written in the same transaction as the sync; no schema change) and removing a root removes its stamp.

## Whole-PC use

Indexing a whole computer is opt-in: nothing is scanned until you register a root and sync it. To index a drive: `flux root add D:\` then `flux sync D:\` (a first sync of a large drive can take a long time; progress is printed to the console as `processed N of M files`, and only the first 10 warnings are shown unless you pass `--verbose`).

Flux protects you from the usual hazards:

- **System and application folders are skipped** unless you pass `--include-system`: `AppData`, `$Recycle.Bin` and `System Volume Information` anywhere below a root, and `Windows`, `Program Files`, `Program Files (x86)`, `ProgramData` and `Recovery` directly under a drive root. A root you choose yourself is never excluded (registering an `AppData` folder indexes it). Because the policy applies to every sync, files in a folder that becomes excluded are removed from the index at the next sync.
- **Links are never followed:** directory symlinks and Windows junctions are skipped (and counted as "Links not followed"), so a link cycle cannot make a scan loop or wander outside the root. (Symlinked *files* are treated as the file they point to.)
- **Access denied** on a folder or file is reported as a warning and never taken as proof that its files were deleted.
- **Removable and network drives:** a root on a drive that is not connected is listed as `NOT AVAILABLE`, is skipped by `sync --all`, makes `sync <dir>` fail without changing anything, and keeps its index entries (so you can still search it). `flux root remove` can forget it. Reconnect and sync again to refresh it.
- **Files that vanish during a scan** are noticed individually; binary files, files over 16 MiB and unsupported extensions are skipped and reported.
- **Unicode:** file and folder names are stored as UTF-8 and searched correctly; command-line arguments are read from the Windows wide command line, and the console is switched to UTF-8 while Flux runs. Very long paths (over 260 characters) were not verified.

Not covered: network shares, cloud-only (not downloaded) files, file formats other than the plain-text types above, and any kind of watching for changes — run `sync` when you want the index refreshed.

## Build

```powershell
.\build-flux.bat                 # Release build into build-release\ (finds CMake and MSYS2 UCRT64 if they are not on PATH)
```

or by hand (CMake 3.16+, a C++17 compiler, and a C compiler for the bundled SQLite):

```powershell
$env:Path = "C:\Program Files\CMake\bin;C:\msys64\ucrt64\bin;$env:Path"
cmake -S . -B build-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 8
```

Use a Release build for real use and for benchmarks; a build with no `CMAKE_BUILD_TYPE` has no optimization. `build\` (plain `cmake -S . -B build`) works for development and tests.

## Package, install and distribute (Windows x64)

Version: set once, in `project(Flux VERSION x.y.z ...)` in `CMakeLists.txt`; it feeds `flux --version`, the About box, the Windows file properties of both programs, and the package and installer names.

**Portable distribution** (needs CMake and MSYS2 UCRT64 as in [Build](#build)):

```powershell
.\packaging\package.ps1
```

It builds Release into `build-dist\` (the compiler runtime is linked in, so the programs need only Windows itself — no MinGW DLLs), stages `dist\Flux\` (`flux-gui.exe`, `flux.exe`, `flux-cmd.bat`, `README.txt`, `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt`, `VERSION.txt`; never a database, sample file, test or benchmark), creates `dist\Flux-<version>-windows-x64.zip`, and runs `packaging\verify-package.ps1` on the extracted archive in a minimal environment with a fresh, isolated `LOCALAPPDATA`. It exits non-zero if any step fails, deletes only what it made itself (a `.flux-package` marker proves it), and never touches your real database. Unzip anywhere and run `flux-gui.exe`; `flux-cmd.bat` opens a console with `flux` ready.

**Installer** (optional): `packaging\flux.iss` is an Inno Setup 6 script (per-user, no administrator rights, `%LOCALAPPDATA%\Programs\Flux`, Start Menu shortcuts, optional desktop shortcut, uninstaller). `package.ps1` compiles it automatically **only if Inno Setup's `ISCC.exe` is installed**, producing `dist\Flux-Setup-<version>-x64.exe`; it never downloads or installs anything. Without Inno Setup it prints that no installer was built.

**Your data** lives in `%LOCALAPPDATA%\Flux\` (database `flux.db`, `gui-settings.ini`), never in the program folder. Upgrading (unzip over, or run a newer installer) keeps your index and settings; an older database is upgraded with a `.v1.bak` backup as described above. Uninstalling removes the program only and leaves that folder; delete it by hand for a clean slate. Flux scans nothing until you add a folder.

**Requirements:** 64-bit Windows 10 (1607+) or 11. **Before distributing**, replace the placeholder `packaging\LICENSE.txt` with your chosen license.

## Test

```powershell
ctest --test-dir build --output-on-failure              # single-config generators
ctest --test-dir build -C Release --output-on-failure   # Visual Studio
```

25 CTest tests (`test_extract` reads every supported document format from real files that each contain a unique word and checks the words, page/slide/sheet labels, passwords, damaged and empty files; `test_content` runs those documents through the real sync into a temporary database and searches them: snippets and locations, statistics, incremental sync, re-extraction of old records, the migration and its backup, unreadable files, a real Windows access-denied folder, cancellation, several roots and large documents; `test_drives` covers drive discovery, drive status, exclusions, pause and the last-sync stamp; `test_gui_smoke`, the launcher test and `test_package`, which stages, zips and verifies the distribution, open a real window briefly): tokenizer, scanner (system-folder policy, Windows junction cycles, Unicode names), queue, worker pool, options, path handling, the database (schema, **schema 1 → 2 upgrade against the real old layout, backups**), the writer (batching, removal, root scoping, abort, rollback), search (ranking, root filter), roots (registration, overlap, removal, isolation), sync (every scenario in [Sync](#sync-what-counts-as-changed)), default-database selection, the whole command line (`runCli`: every command, exit codes, messages, a fake user environment), and `test_windows_launcher`, a PowerShell script that runs `run-flux.bat` and `flux.exe` as real processes (argument forwarding, exit codes, a different working directory, paths with spaces, drive-letter case, default database in a fake `LOCALAPPDATA`, explicit `--db`). All tests use temporary folders and databases; none can touch your real database (the launcher test verifies that at the end). No test depends on sleeps.

## Benchmarks
`build-release\flux_bench.exe` generates a deterministic synthetic corpus and measures indexing, thread scaling, search latency and per-stage cost; it is never run by CTest. See [benchmarks/README.md](benchmarks/README.md).

## Layout

```text
include/flux/   public headers          src/          implementation (cli.cpp holds the commands)
tests/          CTest executables       benchmarks/   flux_bench tool and results
third_party/    vendored SQLite         sample_docs/  small corpus for trying the older command
gui/            Windows desktop app     run-flux-gui.bat  starts it
run-flux.bat    launcher                build-flux.bat  one-time Release build
packaging/      package.ps1, verify-package.ps1, flux.iss, shipped documents
docs/           Milestone 10 audit (gap analysis, measurements)
```

## Current limitations

- Search is exact-term only (no prefix, phrase or fuzzy matching), ranked by raw word frequency. Only the file types listed under [Searching inside documents](#searching-inside-documents) are read.
- No OCR: scanned PDFs and pictures are searchable by name only. The old binary Office formats (`.doc .xls .ppt`) and Outlook `.msg` files are not read. Password-protected files are never opened. The PDF reader is Flux's own: it matched `pdftotext` on 98 % of the words over 53 real PDFs, but fonts without a character map (some scanned-then-OCRed or symbol-encoded files) can come out empty.
- `sync` is on demand; nothing watches the disk. One writer serializes all database writes (with a database, indexing stops scaling beyond about 4 threads; see the benchmark README).
- A sync holds SQLite's write lock from its first write until its commit; a second Flux process writing to the same database at the same time would wait up to 5 seconds and then fail. Several processes were not tested together.
- Tokenizer is ASCII-only: non-ASCII bytes (accented letters, CJK) act as separators, so words in those scripts are not searchable even though the file names are handled.
- Overlapping roots are refused; moving or renaming a root folder means removing the root and adding it again.
- Verified only with GCC/libstdc++ on Windows 11 (MinGW Makefiles). MSVC, Linux, sanitizers and race detectors were not run. The file-clock conversion used for change detection and the default-database paths on other operating systems are untested.
- Not verified: killing Flux in the middle of a sync, network or FAT volumes, paths over 260 characters, non-ASCII command-line arguments typed in a console (the wide command line is read, but this was exercised through library calls and the PowerShell test, not by hand-typing).
- Performance figures exist only for one machine and a synthetic dataset; see the benchmark README.

- The desktop app is Windows-only and keeps no results between sessions; each search is a new read of the database. Long folder paths are shortened in the middle in its lists (the full path is shown when a folder is selected or a result is opened).

## Planned
Later (not started): richer search, filesystem watching (`inotify` on Linux). Not done: the installer script has never been compiled or run (Inno Setup was not available), and the package is unsigned (Windows SmartScreen may warn).
