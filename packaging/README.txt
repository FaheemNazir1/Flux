FLUX
====

Flux searches the contents of the folders you choose. You add a folder, Flux reads its text
files into a small index on your computer, and then you find files by the words inside them,
across all your folders at once. Nothing leaves your computer.

Flux searches ONLY the folders you add. Every file can be found by its NAME. Words INSIDE a file are
searchable for: text and source code (.txt .md .csv .json .py .cpp ...), web pages, PDF, Word (.docx),
PowerPoint (.pptx), Excel (.xlsx), OpenDocument (.odt .ods .odp), RTF, e-mail (.eml), e-books (.epub)
and ZIP archives holding such files. Results show the page, slide or sheet where the words were found.
Password-protected files are found by name only (Flux never tries to open or break a password), and
scanned pictures need OCR, which Flux does not have. View > Content status shows what is searchable.
It never scans anything by itself: you choose every folder.


QUICK START (desktop app)
-------------------------
1. Start "Flux" from the Start Menu (or double-click flux-gui.exe in this folder).
2. Click "Add folder..." and choose a folder, for example your Documents.
3. When asked "Index it now?", click Yes. Flux reads the files (you can cancel any time).
4. Type one or more words in the search box and press Enter. Every word must appear in a
   file for it to match. Double-click a result to open it.
5. When files change, select the folder and click "Sync selected" (or "Sync all"). Flux
   adds new files, updates changed ones and forgets deleted ones.

Keyboard: Ctrl+F search box, Enter search, Ctrl+O add folder, Ctrl+S sync selected folder,
Ctrl+Shift+S sync all, Delete remove the selected folder from Flux (asks first), F5 refresh.
Removing a folder from Flux never deletes your files; it only forgets what was indexed.


QUICK START (command line, for advanced users)
----------------------------------------------
Open "Flux command line" from the Start Menu (or run flux-cmd.bat from this folder). Then:

    flux root add "C:\Users\<you>\Documents"     choose a folder
    flux sync --all                              read its files into the index
    flux search invoice 2024                     find files containing all these words
    flux status                                  what is indexed
    flux help                                    all commands

(Replace <you> and the path with your own folder.) flux.exe can also be used from any other
console by typing its full path, or by adding this folder to your PATH yourself; Flux never
changes your PATH.


WHERE YOUR DATA IS
------------------
Your index and settings live in  %LOCALAPPDATA%\Flux  (usually
C:\Users\<you>\AppData\Local\Flux):

    flux.db            the index (and the folders you registered)
    gui-settings.ini   the desktop app's settings

The desktop app and the command line use the same index. This folder is separate from the
program folder, so upgrading or uninstalling the program does not touch it.

To back up your index, copy flux.db while Flux is not running. To start over, delete flux.db.
Your own files are never modified by Flux.


UPGRADING AND UNINSTALLING
--------------------------
Install a newer version over the old one (or unzip it over the old folder). Your index and
settings are kept. If a newer version changes the database format, it makes a copy of your
database next to it first (flux.db.v1.bak) and then upgrades it.

Uninstalling removes the program only. It does NOT delete %LOCALAPPDATA%\Flux, so your index
and settings survive; delete that folder yourself if you want them gone.


REQUIREMENTS
------------
Windows 10 (version 1607 or later) or Windows 11, 64-bit. Nothing else needs to be installed:
flux.exe and flux-gui.exe use only Windows system libraries.


LICENSE
-------
See LICENSE.txt for Flux itself and THIRD-PARTY-NOTICES.txt for the components it includes
(SQLite and the compiler runtime).
