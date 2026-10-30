# End-to-end tests of run-flux.bat and the flux.exe command line as real processes.
#
# Everything runs against throw-away folders under %TEMP% and a fake LOCALAPPDATA, so the
# real default database of the user running the tests is never read or written (this is
# checked at the end). Run by CTest as test_windows_launcher; can also be run by hand:
#
#   powershell -ExecutionPolicy Bypass -File tests\test_windows_launcher.ps1 `
#       -Launcher .\run-flux.bat -FluxExe .\build\flux.exe
param(
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$FluxExe,
    [string]$GuiExe = ''   # when given, run-flux-gui.bat is tested too (a real window opens briefly)
)
$ErrorActionPreference = 'Stop'
$Launcher = (Resolve-Path $Launcher).Path
$FluxExe = (Resolve-Path $FluxExe).Path

$script:failures = 0
$script:checks = 0
function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Host "  FAIL: $what" -ForegroundColor Red }
}

# Quotes one argument the way Windows command lines expect: also for the characters cmd.exe treats
# specially when a .bat forwards its arguments (& | < > ^ ( ) %), and with backslashes before a quote
# doubled so that a path ending in "\" does not swallow its closing quote.
function Quote-Argument([string]$a) {
    if ($a -ne '' -and $a -notmatch '[\s"&|<>^()%]') { return $a }
    $escaped = $a -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

# Runs a program with exact arguments and returns exit code, stdout and stderr.
function Invoke-Program([string]$file, [string[]]$arguments, [hashtable]$environment = @{}, [string]$workingDirectory = $null) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $file
    $psi.Arguments = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    if ($workingDirectory) { $psi.WorkingDirectory = $workingDirectory }
    foreach ($name in $environment.Keys) {
        if ($null -eq $environment[$name]) { [void]$psi.EnvironmentVariables.Remove($name) }
        else { $psi.EnvironmentVariables[$name] = [string]$environment[$name] }
    }
    $process = [System.Diagnostics.Process]::Start($psi)
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [pscustomobject]@{ Code = $process.ExitCode; Out = $out.Result; Err = $err.Result }
}

# Touch nothing of the real user's data: remember the real default database to prove that at the end.
$realDefault = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Flux\flux.db' } else { $null }
$realBefore = if ($realDefault -and (Test-Path $realDefault)) { (Get-Item $realDefault).LastWriteTimeUtc.Ticks } else { 0 }

$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) ("flux_launcher_test_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $sandbox | Out-Null
$elsewhere = Join-Path $sandbox 'some other working directory'   # a different cwd, with spaces
New-Item -ItemType Directory $elsewhere | Out-Null
$fakeLocal = Join-Path $sandbox 'LocalAppData'
$userEnv = @{ LOCALAPPDATA = $fakeLocal; FLUX_EXE = $FluxExe; FLUX_DB = $null }

try {
    Write-Host 'launcher: argument forwarding and exit codes'
    # A stub stands in for flux.exe so the launcher's own behaviour is observed exactly.
    $stub = Join-Path $sandbox 'stub flux.cmd'
    Set-Content -Path $stub -Value "@echo off`r`necho ARGS=%*`r`necho CWD=%CD%`r`nexit /b 7" -Encoding ASCII
    $r = Invoke-Program $Launcher @('first', 'second arg', 'a&b', '--flag=x y') @{ FLUX_EXE = $stub } $elsewhere
    Check ($r.Code -eq 7) "exit code of the program is returned unchanged (got $($r.Code))"
    Check ($r.Out -match [regex]::Escape('ARGS=first "second arg" "a&b" "--flag=x y"')) "arguments with spaces and & arrive intact: $($r.Out.Trim())"
    Check ($r.Out -match [regex]::Escape("CWD=$elsewhere")) 'the caller''s working directory is preserved'

    $r = Invoke-Program $Launcher @('x') @{ FLUX_EXE = (Join-Path $sandbox 'missing.exe') } $elsewhere
    Check ($r.Code -eq 1) "a missing executable gives exit code 1 (got $($r.Code))"
    Check ($r.Err -match 'could not find flux.exe' -and $r.Err -match 'build-flux.bat') 'and says how to build it'

    $r = Invoke-Program $Launcher @('help') @{ FLUX_EXE = $FluxExe } $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'Quick start') 'launcher runs the real flux.exe from another directory'
    $r = Invoke-Program $Launcher @('search') $userEnv $elsewhere
    Check ($r.Code -eq 2) "a usage error is exit code 2 through the launcher (got $($r.Code))"
    $r = Invoke-Program $Launcher @('status', '--db', (Join-Path $sandbox 'nope.db')) $userEnv $elsewhere
    Check ($r.Code -eq 1 -and $r.Err -match 'no index yet') 'a runtime error is exit code 1 through the launcher'

    # Without FLUX_EXE the launcher finds the build next to it (only checkable if a build exists).
    $repo = Split-Path $Launcher
    if ((Test-Path (Join-Path $repo 'build-release\flux.exe')) -or (Test-Path (Join-Path $repo 'build\flux.exe'))) {
        $r = Invoke-Program $Launcher @('--help') @{ FLUX_EXE = $null } $elsewhere   # --help works in every version
        Check ($r.Code -eq 0 -and $r.Out -match 'Usage|Quick start') 'finds build-release\flux.exe or build\flux.exe by itself'
    }

    Write-Host 'workflow through the launcher: two roots, default database, search across both'
    $docs = Join-Path $sandbox 'My Documents'
    $proj = Join-Path $sandbox 'projects'
    New-Item -ItemType Directory (Join-Path $docs 'notes') -Force | Out-Null
    New-Item -ItemType Directory $proj | Out-Null
    Set-Content (Join-Path $docs 'invoice.txt') 'invoice for acme' -NoNewline
    Set-Content (Join-Path $docs 'notes\todo.md') 'pay the invoice' -NoNewline
    Set-Content (Join-Path $proj 'readme.md') 'acme project' -NoNewline
    Get-ChildItem $sandbox -Recurse -File | ForEach-Object { $_.LastWriteTime = (Get-Date).AddHours(-2) }

    $r = Invoke-Program $Launcher @('root', 'add', $docs) $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'Registered root') "root add (path with spaces): $($r.Err)"
    $defaultDb = Join-Path $fakeLocal 'Flux\flux.db'
    Check (Test-Path $defaultDb) 'the database was created under LOCALAPPDATA\Flux, not in the working directory'
    Check (-not (Test-Path (Join-Path $elsewhere 'flux.db'))) 'nothing was created in the working directory'
    $r = Invoke-Program $Launcher @('root', 'add', $proj) $userEnv $elsewhere
    Check ($r.Code -eq 0) 'second root added'
    $r = Invoke-Program $Launcher @('sync', '--all') $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match '2 added' -and $r.Out -match '1 added' -and $r.Out -match 'Synced 2 roots') "sync --all indexes both roots: $($r.Err)"
    $r = Invoke-Program $Launcher @('search', 'acme') $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match '2 result' -and $r.Out -match 'invoice.txt' -and $r.Out -match 'readme.md') 'one search finds files from both roots'
    $r = Invoke-Program $Launcher @('search', 'invoice', '--root', $docs.ToLower()) $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match '2 result') '--root filter accepts a differently-cased spelling of the folder'
    $r = Invoke-Program $Launcher @('root', 'add', ($docs.Substring(0, 1).ToLower() + $docs.Substring(1) + '\')) $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'Already registered') 'lower-case drive letter and a trailing backslash name the same root'
    $r = Invoke-Program $Launcher @('status') $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match '3 indexed' -and $r.Out -match 'My Documents') 'status summarises roots and counts'

    Write-Host 'one root changes; the other is untouched'
    Remove-Item (Join-Path $proj 'readme.md')
    Remove-Item (Join-Path $docs 'invoice.txt')
    $r = Invoke-Program $Launcher @('sync', $docs) $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'Removed:\s+1') 'sync of one root removes only its own deleted file'
    $r = Invoke-Program $Launcher @('search', 'acme') $userEnv $elsewhere
    Check ($r.Out -match 'readme.md' -and $r.Out -notmatch 'invoice.txt') 'the other root still lists its (not yet synced) file'
    $r = Invoke-Program $Launcher @('sync', '--all') $userEnv $elsewhere
    Check ($r.Code -eq 0) 'sync --all'
    $r = Invoke-Program $Launcher @('root', 'remove', $proj) $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'not touched') 'root remove'

    if ($GuiExe) {
        Write-Host 'GUI launcher (opens the real window briefly, against a throw-away database)'
        $GuiExe = (Resolve-Path $GuiExe).Path
        $guiLauncher = Join-Path (Split-Path $Launcher) 'run-flux-gui.bat'
        $existing = @(Get-Process -Name 'flux-gui' -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })  # never touch the user's own windows
        $guiEnv = @{ FLUX_GUI_EXE = $GuiExe; LOCALAPPDATA = $fakeLocal; FLUX_DB = (Join-Path $sandbox 'gui-launch.db'); FLUX_GUI_SETTINGS = (Join-Path $sandbox 'gui-launch.ini') }

        # Started WITHOUT redirected output: the launcher hands its handles to the app it starts, so capturing
        # them would make this test wait until the window is closed. The isolation variables go through this
        # process's environment (and are restored right after).
        $saved = @{}
        foreach ($name in $guiEnv.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name); [Environment]::SetEnvironmentVariable($name, [string]$guiEnv[$name]) }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        try {
            $launch = Start-Process -FilePath $guiLauncher -WorkingDirectory $elsewhere -WindowStyle Hidden -PassThru
            $returned = $launch.WaitForExit(10000)
        }
        finally {
            foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
        }
        Check ($returned -and $launch.ExitCode -eq 0 -and $timer.Elapsed.TotalSeconds -lt 10) 'run-flux-gui.bat returns at once with exit code 0 (it does not wait for the window)'

        $started = $null
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and -not $started) {
            $started = Get-Process -Name 'flux-gui' -ErrorAction SilentlyContinue |
                Where-Object { $existing -notcontains $_.Id -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
            if (-not $started) { Start-Sleep -Milliseconds 200 }
        }
        Check ($null -ne $started) 'the app window opens'
        if ($started) {
            Check ($started.MainWindowTitle -eq 'Flux') "the window is titled Flux (got '$($started.MainWindowTitle)')"
            Start-Sleep -Milliseconds 800
            $started.Refresh()
            Check ($started.Responding) 'the window is responding'
            [void]$started.CloseMainWindow()
            Check ($started.WaitForExit(15000)) 'closing the window ends the process'
            # (The exit code of a process this script did not start cannot be read; clean shutdown, including
            # closing in the middle of a sync, is verified by the in-process self-test test_gui_smoke.)
        }
        # Anything left over from this test (never the user's own processes) is removed.
        Get-Process -Name 'flux-gui' -ErrorAction SilentlyContinue | Where-Object { $existing -notcontains $_.Id } |
            ForEach-Object { $_.Kill() }
        Check (-not (Test-Path (Join-Path $sandbox 'gui-launch.db')) -and -not (Test-Path (Join-Path $sandbox 'gui-launch.ini'))) 'merely opening (and closing) the app creates no database and no settings file'

        $r = Invoke-Program $guiLauncher @() @{ FLUX_GUI_EXE = (Join-Path $sandbox 'missing-gui.exe') } $elsewhere
        Check ($r.Code -eq 1 -and $r.Err -match 'could not find flux-gui.exe' -and $r.Err -match 'build-flux.bat') 'a missing app gives exit code 1 and says how to build it'
    }

    Write-Host 'explicit --db override'
    $custom = Join-Path $sandbox 'custom location\mine.db'
    New-Item -ItemType Directory (Split-Path $custom) | Out-Null
    $r = Invoke-Program $Launcher @('--db', $custom, $docs) $userEnv $elsewhere
    Check ($r.Code -eq 0 -and (Test-Path $custom)) 'legacy form with --db still stores into the chosen file'
    $r = Invoke-Program $Launcher @('search', '--db', $custom, 'invoice') $userEnv $elsewhere
    Check ($r.Code -eq 0 -and $r.Out -match 'todo.md') 'search --db reads the chosen file'
}
finally {
    Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}

# The real default database must be exactly as it was.
if ($realDefault) {
    $realAfter = if (Test-Path $realDefault) { (Get-Item $realDefault).LastWriteTimeUtc.Ticks } else { 0 }
    Check ($realAfter -eq $realBefore) 'the real default database was not touched by these tests'
}

Write-Host ("{0} checks, {1} failed" -f $script:checks, $script:failures)
if ($script:failures -gt 0) { exit 1 }
exit 0
