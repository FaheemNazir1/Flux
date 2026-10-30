# Runs the desktop app's built-in self-test (flux-gui.exe --smoke-test) and reports its result.
#
# The self-test creates the real window off-screen, drives it through the same code paths the buttons
# use, checks what the real list controls show, probes responsiveness during a long sync, cancels a sync,
# and closes the window in the middle of a sync, then verifies the database. It works entirely inside
# a throw-away folder; the user's database and settings are never touched (checked below).
#
#   powershell -ExecutionPolicy Bypass -File tests\test_gui_smoke.ps1 -GuiExe .\build\flux-gui.exe
#   ... -Screenshots C:\somewhere     keeps BMP screenshots of the window there
param(
    [Parameter(Mandatory = $true)][string]$GuiExe,
    [string]$Screenshots = ''
)
$ErrorActionPreference = 'Stop'
$GuiExe = (Resolve-Path $GuiExe).Path

$realDb = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Flux\flux.db' } else { $null }
$realIni = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Flux\gui-settings.ini' } else { $null }
function Stamp($p) { if ($p -and (Test-Path $p)) { (Get-Item $p).LastWriteTimeUtc.Ticks } else { 0 } }
$before = @{ db = Stamp $realDb; ini = Stamp $realIni }

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('flux_gui_smoke_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
$arguments = @('--smoke-test', "`"$work`"")
if ($Screenshots) { New-Item -ItemType Directory $Screenshots -Force | Out-Null; $arguments += @('--screenshots', "`"$Screenshots`"") }

$exitCode = 1
try {
    $process = Start-Process -FilePath $GuiExe -ArgumentList ($arguments -join ' ') -PassThru
    if (-not $process.WaitForExit(540000)) {
        $process.Kill()
        Write-Host 'FAIL  the GUI self-test did not finish within 9 minutes (killed)' -ForegroundColor Red
    } else {
        $exitCode = $process.ExitCode
    }
    $report = Join-Path $work 'smoke-result.txt'
    if (Test-Path $report) { Get-Content $report | ForEach-Object { Write-Host $_ } }
    else { Write-Host 'FAIL  the self-test wrote no report' -ForegroundColor Red; $exitCode = 1 }
    Write-Host "GUI self-test exit code: $exitCode (0 = every check passed)"

    $after = @{ db = Stamp $realDb; ini = Stamp $realIni }
    if ($after.db -ne $before.db -or $after.ini -ne $before.ini) {
        Write-Host 'FAIL  the real default database or settings file was modified' -ForegroundColor Red
        $exitCode = 1
    }
}
finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
exit $exitCode
