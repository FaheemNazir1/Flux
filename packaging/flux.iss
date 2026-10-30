; Inno Setup script for Flux (https://jrsoftware.org/isinfo.php). Needs Inno Setup 6.3 or newer.
;
; Build it with packaging\package.ps1, which stages the files and calls the compiler for you, or by hand:
;   ISCC.exe /DMyAppVersion=<version> /DStageDir=<path to dist\Flux> /DOutputDir=<folder for the setup exe> flux.iss
;
; The version is NOT written here: it comes from `project(Flux VERSION ...)` in CMakeLists.txt, which
; package.ps1 passes in. Compiling without it is an error on purpose.
;
; What this installer does and does not do:
;   * installs for the CURRENT USER only (no administrator rights, no UAC prompt), by default into
;     %LOCALAPPDATA%\Programs\Flux
;   * adds a Start Menu folder (Flux, Flux command line, Uninstall Flux) and, if you tick it, a desktop shortcut
;   * registers an uninstaller (Settings > Apps) and upgrades in place when run again
;   * never touches your data: the index database, the folders you registered and the settings live in
;     %LOCALAPPDATA%\Flux, which this script neither installs into, overwrites, nor removes
;   * installs no service, no scheduled task, no startup entry, and does not change PATH

#ifndef MyAppVersion
  #error MyAppVersion is not defined. Pass /DMyAppVersion=<version> (packaging\package.ps1 reads it from CMakeLists.txt).
#endif
#ifndef StageDir
  #error StageDir is not defined. Pass /DStageDir=<path to the staged dist\Flux folder>.
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define MyAppName "Flux"

[Setup]
; The identity of Flux for upgrades and uninstall. Never change it once a release has been published.
AppId={{961C033A-1F3D-4EDE-AAAB-3680AEFED4C4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
UsePreviousAppDir=yes
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Flux-Setup-{#MyAppVersion}-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\flux-gui.exe
; If Flux is open during an upgrade, offer to close it instead of failing on a locked file.
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; Only the staged program files. There is no database, sample data or user configuration in the package.
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Flux"; Filename: "{app}\flux-gui.exe"
Name: "{group}\Flux command line"; Filename: "{app}\flux-cmd.bat"; WorkingDir: "{userdocs}"
Name: "{group}\Uninstall Flux"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Flux"; Filename: "{app}\flux-gui.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\flux-gui.exe"; Description: "Start Flux"; Flags: nowait postinstall skipifsilent

; Deliberately absent: [UninstallDelete], [Registry], [Services], scheduled tasks. The uninstaller removes
; exactly the files this installer put in the program folder, and nothing in %LOCALAPPDATA%\Flux.
