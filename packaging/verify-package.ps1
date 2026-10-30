# Verifies a staged Flux distribution and its ZIP archive. Used by package.ps1 and by the CTest test
# test_package; can be run by hand:
#
#   powershell -ExecutionPolicy Bypass -File packaging\verify-package.ps1 `
#       -StageDir dist\Flux -Zip dist\Flux-0.9.0-windows-x64.zip -Version 0.9.0
#
# What it proves (and what it does not):
#   * the package has exactly the expected files and none of the forbidden ones (no database, sample
#     data, build leftovers, test programs or DLLs);
#   * the ZIP holds exactly the same bytes under a single "Flux/" folder;
#   * the programs carry the right version and run from a COPY of the package, in a minimal environment
#     (PATH contains only the Windows folder, so no compiler DLLs can be found), from another working
#     directory, with a fresh and isolated %LOCALAPPDATA%;
#   * the CLI and the desktop app use the default database location, create it only when appropriate,
#     and leave an existing database byte for byte unchanged;
#   * the installer script keeps the promises made about it (static checks; it is compiled only when an
#     Inno Setup compiler is supplied).
# It never uses the real user database: every program runs against a throw-away %LOCALAPPDATA%.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StageDir,
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$InstallerScript = '',
    [string]$IsccPath = '',            # when given, the installer script is also compiled (into a temp folder)
    [switch]$SkipGuiSelfTest           # skip the ~20 s built-in GUI self-test
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not $InstallerScript) { $InstallerScript = Join-Path $PSScriptRoot 'flux.iss' }
$StageDir = (Resolve-Path $StageDir).Path
$Zip = (Resolve-Path $Zip).Path

$script:checks = 0
$script:failures = 0
$script:warnings = 0
function Check([bool]$condition, [string]$what) {
    $script:checks++
    if ($condition) { Write-Host "  PASS  $what" }
    else { $script:failures++; Write-Host "  FAIL  $what" -ForegroundColor Red }
}
function Warn([string]$what) { $script:warnings++; Write-Host "  WARN  $what" -ForegroundColor Yellow }
function Info([string]$what) { Write-Host "        $what" }
function Section([string]$title) { Write-Host ''; Write-Host $title }

function Quote-Argument([string]$a) {
    if ($a -ne '' -and $a -notmatch '[\s"]') { return $a }
    $escaped = $a -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

# The environment every program under test gets: nothing but what Windows itself needs. In particular PATH
# is just the Windows folder, so a missing compiler runtime DLL would make the program fail to start.
function New-MinimalStartInfo([string]$exe, [string[]]$arguments, [string]$localAppData, [string]$workingDirectory, [bool]$redirect) {
    $system = $env:SystemRoot
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $workingDirectory
    if ($redirect) {
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
        $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    }
    $psi.EnvironmentVariables.Clear()
    $profile = Join-Path (Split-Path $localAppData -Parent) 'profile'
    $values = [ordered]@{
        SystemRoot = $system; windir = $system; PATH = (Join-Path $system 'System32')
        TEMP = $script:tempRoot; TMP = $script:tempRoot; LOCALAPPDATA = $localAppData; USERPROFILE = $profile
    }
    foreach ($name in $values.Keys) { $psi.EnvironmentVariables[$name] = [string]$values[$name] }
    return $psi
}

function Invoke-Minimal([string]$exe, [string[]]$arguments, [string]$localAppData, [string]$workingDirectory, [int]$timeoutMs = 120000) {
    $psi = New-MinimalStartInfo $exe $arguments $localAppData $workingDirectory $true
    $process = [System.Diagnostics.Process]::Start($psi)
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($timeoutMs)) {
        $process.Kill()
        return [pscustomobject]@{ Code = -1; Out = ''; Err = "timed out after $timeoutMs ms"; TimedOut = $true }
    }
    $process.WaitForExit()
    return [pscustomobject]@{ Code = $process.ExitCode; Out = $out.Result; Err = $err.Result; TimedOut = $false }
}

function Hash-Of([string]$path) { if (Test-Path $path) { (Get-FileHash $path -Algorithm SHA256).Hash } else { '(missing)' } }

# --------------------------------------------------------------------------------------------- setup
$script:tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('flux_package_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $script:tempRoot | Out-Null

# The real user database is never used; if it exists we only note whether anything changed it meanwhile.
$realDb = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Flux\flux.db' } else { $null }
function RealDbStamp { if ($realDb -and (Test-Path $realDb)) { "$((Get-Item $realDb).LastWriteTimeUtc.Ticks)/$((Get-Item $realDb).Length)" } else { 'absent' } }
$realBefore = RealDbStamp

try {
    # ----------------------------------------------------------------------------- 1. staged contents
    Section '1. Staged package contents'
    $expected = @('flux.exe', 'flux-gui.exe', 'flux-cmd.bat', 'README.txt', 'THIRD-PARTY-NOTICES.txt', 'LICENSE.txt', 'VERSION.txt')
    $stageFiles = @(Get-ChildItem $StageDir -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($StageDir.Length + 1).Replace('\', '/') } | Sort-Object)
    Check (($stageFiles -join ',') -eq (($expected | Sort-Object) -join ',')) "the package holds exactly: $($expected -join ', ')"
    $forbidden = @($stageFiles | Where-Object { $_ -match '\.(db|db-wal|db-shm|sqlite|o|obj|a|lib|pdb|ilk|dll|cmake|ninja)$' -or $_ -match '^test_|sample_docs|benchmarks|CMakeCache|CMakeFiles|\.git' })
    Check ($forbidden.Count -eq 0) "no database, build leftovers, test programs, DLLs or sample data ($($forbidden -join ', '))"
    $sizeMb = [math]::Round((($stageFiles | ForEach-Object { (Get-Item (Join-Path $StageDir $_)).Length } | Measure-Object -Sum).Sum) / 1MB, 1)
    Info "total size $sizeMb MB"
    Check ($sizeMb -lt 40) 'the package is small (under 40 MB), so no dataset or toolchain got in'
    $cmdBytes = [System.IO.File]::ReadAllBytes((Join-Path $StageDir 'flux-cmd.bat'))
    $lf = @($cmdBytes | Where-Object { $_ -eq 10 }).Count
    $crlf = 0; for ($i = 1; $i -lt $cmdBytes.Length; $i++) { if ($cmdBytes[$i] -eq 10 -and $cmdBytes[$i - 1] -eq 13) { $crlf++ } }
    Check ($lf -gt 0 -and $lf -eq $crlf) 'flux-cmd.bat uses Windows line endings'
    $versionText = Get-Content (Join-Path $StageDir 'VERSION.txt') -Raw
    Check ($versionText -match [regex]::Escape($Version)) "VERSION.txt names version $Version"
    $license = Get-Content (Join-Path $StageDir 'LICENSE.txt') -Raw
    Check ($license.Length -gt 50) 'LICENSE.txt is present'
    $textFiles = @($stageFiles | Where-Object { $_ -match '\.(txt|bat)$' })
    $privateHints = @($textFiles | Where-Object { (Get-Content (Join-Path $StageDir $_) -Raw) -match [regex]::Escape($env:USERNAME) })
    Check ($env:USERNAME.Length -lt 4 -or $privateHints.Count -eq 0) 'no text file in the package contains this machine''s user name'
    foreach ($name in 'flux.exe', 'flux-gui.exe') {  # information only: build-machine paths in a binary are harmless but worth knowing about
        $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes((Join-Path $StageDir $name)))
        $leaks = @($env:USERNAME, 'D:\Project', 'D:/Project') | Where-Object { $_ -and $_.Length -gt 3 -and $ascii.Contains($_) }
        if ($leaks) { Info "note: $name contains build-machine text ($($leaks -join ', ')); consider building with -ffile-prefix-map to remove it" }
    }

    # --------------------------------------------------------------------------------- 2. identity
    Section '2. Executable identity'
    foreach ($name in 'flux.exe', 'flux-gui.exe') {
        $info = (Get-Item (Join-Path $StageDir $name)).VersionInfo
        Check ($info.ProductName -eq 'Flux' -and $info.FileVersion -eq $Version -and $info.ProductVersion -eq $Version) "$name is Flux $Version (ProductName/FileVersion/ProductVersion)"
        Check ($info.OriginalFilename -eq $name -and $info.FileDescription -match 'Flux') "$name has its own name and description ('$($info.FileDescription)')"
    }

    # ------------------------------------------------------------------------------------ 3. ZIP
    Section '3. ZIP archive'
    Check ((Split-Path $Zip -Leaf) -match [regex]::Escape($Version)) "the archive name carries the version ($(Split-Path $Zip -Leaf))"
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $entries = @($archive.Entries | Where-Object { $_.Name -ne '' })
        $names = @($entries | ForEach-Object { $_.FullName })
        Check (@($names | Where-Object { $_ -notmatch '^Flux/[^/]' }).Count -eq 0) 'every entry is inside a single top-level "Flux/" folder'
        Check (@($names | Where-Object { $_ -match '\\' }).Count -eq 0) 'entry names use forward slashes'
        $zipRelative = @($names | ForEach-Object { $_.Substring('Flux/'.Length) } | Sort-Object)
        Check (($zipRelative -join ',') -eq ($stageFiles -join ',')) 'the archive holds exactly the staged files'
    }
    finally { $archive.Dispose() }

    $extractRoot = Join-Path $script:tempRoot 'unzipped'
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $extractRoot)
    $pkg = Join-Path $extractRoot 'Flux'
    $same = $true
    foreach ($file in $stageFiles) { if ((Hash-Of (Join-Path $StageDir $file)) -ne (Hash-Of (Join-Path $pkg $file))) { $same = $false; Info "differs: $file" } }
    Check $same 'extracted files are byte-identical to the staged files (SHA-256)'
    Info "everything below runs from the extracted copy: $pkg"

    # ------------------------------------------------------------------ 4. dependencies / runs alone
    Section '4. Dependencies: the programs need nothing but Windows'
    $objdump = Get-Command objdump -ErrorAction SilentlyContinue
    if ($objdump) {
        foreach ($name in 'flux.exe', 'flux-gui.exe') {
            $imports = @(& $objdump.Source -p (Join-Path $pkg $name) | Select-String 'DLL Name' | ForEach-Object { ($_.Line -replace '.*DLL Name:\s*', '').Trim() })
            $nonSystem = @($imports | Where-Object { $_ -match '^(libgcc|libstdc\+\+|libwinpthread|libgomp|libssp|libsqlite|zlib)' })
            Check ($nonSystem.Count -eq 0) "$name imports no compiler/third-party DLLs ($($imports.Count) imports, all Windows system libraries)"
        }
    } else { Info 'objdump not found: the import tables were not listed (the behavioural checks below still apply)' }

    $fresh = Join-Path $script:tempRoot 'user\LocalAppData'
    New-Item -ItemType Directory $fresh -Force | Out-Null
    $elsewhere = Join-Path $script:tempRoot 'some other folder'
    New-Item -ItemType Directory $elsewhere | Out-Null
    $flux = Join-Path $pkg 'flux.exe'
    $guiExe = Join-Path $pkg 'flux-gui.exe'

    # ------------------------------------------------------------------------- 5. command line
    Section '5. Command line, minimal environment, fresh LOCALAPPDATA, other working directory'
    $r = Invoke-Minimal $flux @('--version') $fresh $elsewhere
    Check ($r.Code -eq 0 -and $r.Out.Trim() -eq "Flux $Version") "flux --version prints 'Flux $Version' (exit $($r.Code))"
    $r = Invoke-Minimal $flux @('help') $fresh $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'Quick start' -and $r.Out -match [regex]::Escape($Version)) 'flux help works'
    $r = Invoke-Minimal $flux @('status') $fresh $elsewhere
    Check ($r.Code -eq 1 -and $r.Err -match 'no index yet') 'on a fresh machine flux status explains that there is no index yet'
    Check (-not (Test-Path (Join-Path $fresh 'Flux'))) 'reading never creates the data folder or a database'

    $data = Join-Path $script:tempRoot 'My Documents'
    New-Item -ItemType Directory (Join-Path $data 'notes') -Force | Out-Null
    Set-Content (Join-Path $data 'invoice.txt') 'invoice for acme total 100' -NoNewline
    Set-Content (Join-Path $data 'notes\todo.md') 'pay the acme invoice' -NoNewline
    Set-Content (Join-Path $data 'letter.txt') 'dear friend' -NoNewline
    Get-ChildItem $data -Recurse -File | ForEach-Object { $_.LastWriteTime = (Get-Date).AddHours(-2) }

    $r = Invoke-Minimal $flux @('root', 'add', $data) $fresh $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'Registered root') "flux root add (path with a space) works: $($r.Err)"
    $defaultDb = Join-Path $fresh 'Flux\flux.db'
    Check (Test-Path $defaultDb) 'the database was created at LOCALAPPDATA\Flux\flux.db (the default location) on first write'
    $r = Invoke-Minimal $flux @('sync', '--all') $fresh $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match '3 added') "flux sync --all indexes the folder: $($r.Err)"
    $r = Invoke-Minimal $flux @('search', 'acme') $fresh $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match '2 result' -and $r.Out -match 'invoice.txt') 'flux search finds the files'
    $r = Invoke-Minimal $flux @('status') $fresh $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'default location' -and $r.Out -match '3 indexed') 'flux status reports the default location and 3 files'
    $dbBefore = Hash-Of $defaultDb
    $sizeBefore = (Get-Item $defaultDb).Length
    Info "the index now exists; its SHA-256 is recorded and must not change below ($($dbBefore.Substring(0, 12))...)"

    # ----------------------------------------------------------------------------- 6. desktop app
    Section '6. Desktop app launched from the package'
    $psi = New-MinimalStartInfo $guiExe @() $fresh $elsewhere $false
    $app = [System.Diagnostics.Process]::Start($psi)
    try {
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Date) -lt $deadline -and $app.MainWindowHandle -eq [IntPtr]::Zero -and -not $app.HasExited) { Start-Sleep -Milliseconds 200; $app.Refresh() }
        Check ($app.MainWindowHandle -ne [IntPtr]::Zero) 'flux-gui.exe opens its window from the extracted package, in a minimal environment'
        if ($app.MainWindowHandle -ne [IntPtr]::Zero) {
            Check ($app.MainWindowTitle -eq 'Flux') "the window is titled 'Flux' (got '$($app.MainWindowTitle)')"
            Start-Sleep -Milliseconds 1200
            $app.Refresh()
            Check $app.Responding 'the window is responding'
            [void]$app.CloseMainWindow()
            Check ($app.WaitForExit(15000)) 'closing the window ends the process'
            if ($app.HasExited) { Check ($app.ExitCode -eq 0) "and it exits with code 0 (got $($app.ExitCode))" }
        }
    }
    finally { if (-not $app.HasExited) { $app.Kill() } }
    Check ((Hash-Of $defaultDb) -eq $dbBefore -and (Get-Item $defaultDb).Length -eq $sizeBefore) 'launching the app left the existing database byte for byte unchanged'
    Check (-not (Test-Path (Join-Path $fresh 'Flux\gui-settings.ini'))) 'merely opening the app wrote no settings file'

    if ($SkipGuiSelfTest) { Info 'GUI self-test skipped (-SkipGuiSelfTest)' }
    else {
        $selfTest = Join-Path $script:tempRoot 'selftest'
        $freshForTest = Join-Path $script:tempRoot 'user2\LocalAppData'
        New-Item -ItemType Directory $freshForTest -Force | Out-Null
        $psi = New-MinimalStartInfo $guiExe @('--smoke-test', $selfTest) $freshForTest $elsewhere $false
        $test = [System.Diagnostics.Process]::Start($psi)
        $finished = $test.WaitForExit(420000)
        if (-not $finished) { $test.Kill() }
        $report = Join-Path $selfTest 'smoke-result.txt'
        $summary = if (Test-Path $report) { @(Get-Content $report | Select-String 'checks:') | Select-Object -Last 1 } else { $null }
        Check ($finished -and $test.ExitCode -eq 0) "the packaged flux-gui.exe passes its built-in self-test ($(if ($summary) { $summary.Line.Trim() } else { 'no report' }))"
        if ($finished -and $test.ExitCode -ne 0 -and (Test-Path $report)) { Get-Content $report | Where-Object { $_ -match '^FAIL' } | ForEach-Object { Info $_ } }
        Check (-not (Test-Path (Join-Path $freshForTest 'Flux'))) 'and the self-test kept to its own folder (the isolated LOCALAPPDATA stayed empty)'
    }

    Section '7. Existing data is preserved'
    $r1 = Invoke-Minimal $flux @('search', 'invoice') $fresh $elsewhere
    $r2 = Invoke-Minimal $flux @('status') $fresh $elsewhere
    Check ($r1.Code -eq 0 -and $r2.Code -eq 0) 'the packaged programs read the existing index'
    Check ((Hash-Of $defaultDb) -eq $dbBefore) 'after every run above, the existing database is byte-identical (SHA-256) to before'
    Check (-not (@(Get-ChildItem $pkg -Recurse -Filter '*.db*' -Force -ErrorAction SilentlyContinue)).Count) 'no database file appeared inside the package folder'

    # ------------------------------------------------------------------------- 8. installer script
    Section '8. Installer script (static checks; compiled only if Inno Setup is available)'
    if (-not (Test-Path $InstallerScript)) { Check $false "installer script exists ($InstallerScript)" }
    else {
        $lines = @(Get-Content $InstallerScript)
        $code = @($lines | Where-Object { $_ -notmatch '^\s*;' -and $_.Trim() -ne '' })
        $text = $code -join "`n"
        Check ($text -match 'PrivilegesRequired=lowest') 'installs per user, no administrator rights needed (PrivilegesRequired=lowest)'
        Check ($text -match 'AppId=\{\{[0-9A-Fa-f-]{36}\}') 'has a fixed AppId so upgrades and uninstall find the same application'
        Check ($text -match '#ifndef MyAppVersion' -and $text -match '#error MyAppVersion') 'refuses to compile without a version (single version source)'
        Check ($text -match 'AppVersion=\{#MyAppVersion\}' -and $text -match 'OutputBaseFilename=Flux-Setup-\{#MyAppVersion\}') 'application and setup file names use the version define'
        Check ($text -notmatch '\b\d+\.\d+\.\d+\b') 'no version number is hard-coded in the script'
        Check ($text -notmatch '(?im)^\[(UninstallDelete|InstallDelete|Registry|Services|UninstallRun)\]') 'no [UninstallDelete]/[InstallDelete]/[Registry]/[Services]: no data deletion, no PATH or startup changes'
        Check ($text -notmatch '(?i)localappdata|userappdata|\.db\b|flux\.db') 'never refers to the data folder or to any database'
        $destDirs = @($code | Where-Object { $_ -match 'DestDir:' } | ForEach-Object { ([regex]::Match($_, 'DestDir:\s*"([^"]+)"')).Groups[1].Value })
        Check ($destDirs.Count -gt 0 -and @($destDirs | Where-Object { $_ -ne '{app}' }).Count -eq 0) 'installs files only into the program folder ({app})'
        Check ($text -match 'Source:\s*"\{#StageDir\}\\\*"') 'installs exactly the staged package folder'
        Check ($text -match 'Name:\s*"\{group\}\\Flux";\s*Filename:\s*"\{app\}\\flux-gui\.exe"') 'has a Start Menu shortcut to the desktop app'
        Check ($text -match 'desktopicon.*Flags:\s*unchecked') 'the desktop shortcut is optional and off by default'
        Check ($text -match '\{uninstallexe\}') 'provides an uninstaller shortcut (an uninstaller is registered by Inno Setup)'
        $runLines = @($code | Where-Object { $_ -match '^Filename:' })
        Check ($runLines.Count -ge 1 -and @($runLines | Where-Object { $_ -notmatch 'postinstall' }).Count -eq 0) 'the only thing run at the end is the optional "Start Flux" checkbox'
        $referenced = @([regex]::Matches($text, '\{app\}\\([A-Za-z0-9._-]+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
        $missing = @($referenced | Where-Object { $stageFiles -notcontains $_ })
        Check ($missing.Count -eq 0) "every program the script refers to is in the package ($($referenced -join ', '))"
        if ($IsccPath) {
            $setupOut = Join-Path $script:tempRoot 'setup-out'
            New-Item -ItemType Directory $setupOut | Out-Null
            & $IsccPath "/DMyAppVersion=$Version" "/DStageDir=$StageDir" "/DOutputDir=$setupOut" $InstallerScript | Out-Host
            $built = @(Get-ChildItem $setupOut -Filter 'Flux-Setup-*.exe' -ErrorAction SilentlyContinue)
            Check ($LASTEXITCODE -eq 0 -and $built.Count -eq 1 -and $built[0].Length -gt 100000) 'Inno Setup compiled the installer'
        } else { Info 'Inno Setup was not supplied: the installer script was checked statically only and was NOT compiled or run' }
    }

    $realAfter = RealDbStamp
    if ($realAfter -ne $realBefore) { Warn "your real database changed while this ran ($realBefore -> $realAfter). Nothing here uses it (every program had its own isolated LOCALAPPDATA); you were probably using Flux at the same time." }
}
finally {
    Remove-Item $script:tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host ("{0} checks, {1} failed, {2} warning(s)" -f $script:checks, $script:failures, $script:warnings)
if ($script:failures -gt 0) { exit 1 }
exit 0
