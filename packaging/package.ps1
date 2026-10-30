# Builds the Flux Windows distribution: a staged folder, a ZIP archive and (if Inno Setup is available)
# an installer.
#
#   .\packaging\package.ps1                 build Release, stage dist\Flux, make the ZIP, verify it
#
# Output (in dist\ unless -OutputDir says otherwise):
#   Flux\                         the distribution folder (exactly the files a user needs)
#   Flux-<version>-windows-x64.zip   the same files in one archive
#   Flux-Setup-<version>-x64.exe  the installer; ONLY if Inno Setup 6 (ISCC.exe) is installed
#
# Safety:
#   * It never touches your databases, sample documents or source files. It writes the build into
#     build-dist\ and the results into the output folder, and nothing else.
#   * It deletes only what it created itself: the output folder's "Flux" subfolder (and only when a
#     ".flux-package" marker proves an earlier run of this script made it) and its own ZIP/installer names.
#   * It exits with a non-zero code, and says so, if any step fails. It never reports an artifact that
#     does not exist.
#
# Options:
#   -SkipBuild -FluxExe <path> -GuiExe <path>   package binaries you already built (used by the CTest test)
#   -OutputDir <folder>  -BuildDir <folder>     change where things go
#   -SkipVerify  -SkipInstaller  -SkipGuiSelfTest
[CmdletBinding()]
param(
    [string]$OutputDir = '',
    [string]$BuildDir = '',
    [switch]$SkipBuild,
    [string]$FluxExe = '',
    [string]$GuiExe = '',
    [switch]$SkipVerify,
    [switch]$SkipInstaller,
    [switch]$SkipGuiSelfTest
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repo = Split-Path $PSScriptRoot -Parent
$originalPath = $env:Path

function Step([string]$text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Run-Native([string]$what, [scriptblock]$command) {
    & $command
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}
function Full([string]$path) { [System.IO.Path]::GetFullPath($path) }

try {
    # ------------------------------------------------------------------------------------- version
    Step 'Version (single source: project(Flux VERSION ...) in CMakeLists.txt)'
    $cmakeText = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
    $m = [regex]::Match($cmakeText, 'project\(\s*Flux\s+VERSION\s+(\d+\.\d+\.\d+)')
    if (-not $m.Success) { throw 'cannot read the version from CMakeLists.txt (expected: project(Flux VERSION x.y.z ...))' }
    $version = $m.Groups[1].Value
    Write-Host "Flux $version"

    # -------------------------------------------------------------------------------- output folder
    if (-not $OutputDir) { $OutputDir = Join-Path $repo 'dist' }
    $OutputDir = Full $OutputDir
    $repoFull = (Full $repo).TrimEnd('\')
    $out = $OutputDir.TrimEnd('\')
    $protected = @($repoFull, $env:USERPROFILE, $env:LOCALAPPDATA, (Join-Path $env:LOCALAPPDATA 'Flux'), [System.IO.Path]::GetPathRoot($out).TrimEnd('\')) |
        Where-Object { $_ } | ForEach-Object { (Full $_).TrimEnd('\') }
    if ($protected -contains $out) { throw "refusing to use '$OutputDir' as the output folder: it is a protected location (the repository, your profile, your data folder or a drive root). Choose a dedicated folder such as dist." }
    if ($out.StartsWith((Join-Path $env:LOCALAPPDATA 'Flux').TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'refusing to write inside the Flux data folder' }
    if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-dist' }
    $BuildDir = Full $BuildDir

    $stage = Join-Path $out 'Flux'
    $marker = Join-Path $out '.flux-package'
    $zipPath = Join-Path $out "Flux-$version-windows-x64.zip"
    $setupPath = Join-Path $out "Flux-Setup-$version-x64.exe"

    # ------------------------------------------------------------------------------------- build
    if ($SkipBuild) {
        Step 'Build skipped: packaging the binaries that were given'
        if (-not ($FluxExe -and $GuiExe)) { throw '-SkipBuild needs -FluxExe and -GuiExe' }
    }
    else {
        Step "Building Release into $BuildDir"
        foreach ($tool in @(@{ Name = 'cmake'; Dir = 'C:\Program Files\CMake\bin' }, @{ Name = 'g++'; Dir = 'C:\msys64\ucrt64\bin' })) {
            if (-not (Get-Command $tool.Name -ErrorAction SilentlyContinue) -and (Test-Path $tool.Dir)) { $env:Path = "$($tool.Dir);$env:Path" }  # this run only
            if (-not (Get-Command $tool.Name -ErrorAction SilentlyContinue)) { throw "$($tool.Name) was not found. Install it (see README, 'Build from source') or put it on PATH." }
        }
        Run-Native 'CMake configure' { cmake -S $repo -B $BuildDir -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release -DFLUX_STATIC_RUNTIME=ON -DFLUX_BUILD_GUI=ON }
        Run-Native 'CMake build' { cmake --build $BuildDir --target flux flux-gui -j 8 }
        $FluxExe = Join-Path $BuildDir 'flux.exe'
        $GuiExe = Join-Path $BuildDir 'flux-gui.exe'
    }
    foreach ($exe in $FluxExe, $GuiExe) { if (-not (Test-Path $exe)) { throw "program not found: $exe" } }
    $FluxExe = (Resolve-Path $FluxExe).Path
    $GuiExe = (Resolve-Path $GuiExe).Path

    # ------------------------------------------------------------------------------------ stage
    Step "Staging $stage"
    if (Test-Path $stage) {
        if (-not (Test-Path $marker)) { throw "'$stage' already exists but was not made by this script (no .flux-package marker next to it). Remove or rename it yourself, or choose another -OutputDir; nothing was deleted." }
        Remove-Item $stage -Recurse -Force  # ours: made by an earlier run, proven by the marker
    }
    New-Item -ItemType Directory $stage -Force | Out-Null
    Set-Content -Path $marker -Value 'This folder is managed by packaging\package.ps1. It may delete and recreate the "Flux" folder, the Flux-*.zip and the Flux-Setup-*.exe here.' -Encoding ASCII

    Copy-Item $FluxExe (Join-Path $stage 'flux.exe')
    Copy-Item $GuiExe (Join-Path $stage 'flux-gui.exe')
    foreach ($doc in 'README.txt', 'THIRD-PARTY-NOTICES.txt', 'LICENSE.txt', 'flux-cmd.bat') { Copy-Item (Join-Path $PSScriptRoot $doc) (Join-Path $stage $doc) }
    $versionLines = @("Flux $version", 'Platform: Windows x64 (10 version 1607 or later, 11)', 'Runtime: linked into the programs; needs only Windows system libraries')
    [System.IO.File]::WriteAllText((Join-Path $stage 'VERSION.txt'), ($versionLines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
    Get-ChildItem $stage | ForEach-Object { Write-Host ("  {0,-26} {1,10:N0} bytes" -f $_.Name, $_.Length) }

    # -------------------------------------------------------------------------------------- zip
    Step "Creating $zipPath"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }  # this script's own artifact name
    $stream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
    try {
        $archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in (Get-ChildItem $stage -File | Sort-Object Name)) {
                $entry = $archive.CreateEntry("Flux/$($file.Name)", [System.IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = $file.LastWriteTime
                $entryStream = $entry.Open()
                try { $bytes = [System.IO.File]::ReadAllBytes($file.FullName); $entryStream.Write($bytes, 0, $bytes.Length) } finally { $entryStream.Dispose() }
            }
        }
        finally { $archive.Dispose() }
    }
    finally { $stream.Dispose() }
    if (-not (Test-Path $zipPath) -or (Get-Item $zipPath).Length -lt 1000) { throw 'the ZIP archive was not created correctly' }
    Write-Host ("  {0}  {1:N0} bytes" -f $zipPath, (Get-Item $zipPath).Length)

    # ------------------------------------------------------------------------------------ installer
    $iscc = $null
    $installerBuilt = $false
    if ($SkipInstaller) { Step 'Installer skipped (-SkipInstaller)' }
    else {
        Step 'Installer (Inno Setup)'
        $candidates = @((Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object { $_.Source }),
                        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
                        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { $_ -and (Test-Path $_) }
        if ($candidates) {
            $iscc = @($candidates)[0]
            Write-Host "using $iscc"
            if (Test-Path $setupPath) { Remove-Item $setupPath -Force }
            Run-Native 'Inno Setup compile' { & $iscc "/DMyAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$out" (Join-Path $PSScriptRoot 'flux.iss') }
            if (-not (Test-Path $setupPath) -or (Get-Item $setupPath).Length -lt 100000) { throw 'Inno Setup reported success but the installer was not created' }
            $installerBuilt = $true
        }
        else {
            Write-Host 'Inno Setup 6 (ISCC.exe) was not found, so NO installer was built. The ZIP is complete and usable.' -ForegroundColor Yellow
            Write-Host 'To build the installer, install Inno Setup 6.3 or newer from https://jrsoftware.org/isdl.php (this script never downloads or installs it) and run this script again.'
        }
    }

    # ------------------------------------------------------------------------------------- verify
    if ($SkipVerify) { Step 'Verification skipped (-SkipVerify): the package is NOT verified' }
    else {
        Step 'Verifying the package'
        $verifyArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'verify-package.ps1'),
                        '-StageDir', $stage, '-Zip', $zipPath, '-Version', $version)
        if ($iscc) { $verifyArgs += @('-IsccPath', $iscc) }
        if ($SkipGuiSelfTest) { $verifyArgs += '-SkipGuiSelfTest' }
        & (Join-Path $PSHOME 'powershell.exe') @verifyArgs
        if ($LASTEXITCODE -ne 0) { throw 'package verification FAILED (see the FAIL lines above)' }
    }

    # ------------------------------------------------------------------------------------ summary
    Step 'Done'
    Write-Host "Flux $version"
    Write-Host "  Folder   : $stage"
    Write-Host ("  ZIP      : {0}  ({1:N0} bytes, SHA-256 {2})" -f $zipPath, (Get-Item $zipPath).Length, (Get-FileHash $zipPath -Algorithm SHA256).Hash)
    if ($installerBuilt) { Write-Host ("  Installer: {0}  ({1:N0} bytes)" -f $setupPath, (Get-Item $setupPath).Length) }
    else { Write-Host '  Installer: NOT BUILT (Inno Setup not available)' }
    Write-Host ("  Verified : {0}" -f $(if ($SkipVerify) { 'NO (skipped)' } else { 'yes' }))
    exit 0
}
catch {
    Write-Host ''
    Write-Host "PACKAGING FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    $env:Path = $originalPath
}
