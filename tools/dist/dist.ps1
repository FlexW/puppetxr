# tools/dist/dist.ps1 [-NoTests] [-Pin]
# Builds the Windows release (docs/building.md, Windows releases): preset win-clang-dist, the
# DLL import check, then build/dist-windows/out/ holds puppetxr-<version>-windows-x64.zip, the
# matching -pdb.zip and a .buildinfo with the toolchain versions and checksums.
#
# The toolchain is pinned by checking, not by installing: tools/dist/windows-toolchain.json
# names the clang, MSVC, Windows SDK, glslc and PowerShell versions of releases, and the build
# refuses any other. -Pin writes the file from the current toolchain; commit it. Same commit and
# toolchain give byte-identical zips.

#Requires -Version 7.4

param([switch]$NoTests, [switch]$Pin)

$ErrorActionPreference = 'Stop'

$root     = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$pinFile  = Join-Path $PSScriptRoot 'windows-toolchain.json'
$build    = Join-Path $root 'build\dist-windows'
$out      = Join-Path $build 'out'

# Runs a program and stops on its failure, as $ErrorActionPreference does not for native programs
function Invoke-Native
{
    $program, $arguments = $args
    & $program @arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "$program $arguments failed with exit code $LASTEXITCODE"
    }
}

# Entries sorted by path, one fixed time, no directory entries: the bytes depend on the files only
function New-ReproducibleZip
{
    param([string]$Directory, [string]$Prefix, [string]$Path, [DateTimeOffset]$Time)
    Add-Type -AssemblyName System.IO.Compression
    # Ordinal: a culture's collation would make the order depend on the machine's language
    [string[]]$files = Get-ChildItem -Path $Directory -Recurse -File |
        ForEach-Object { [IO.Path]::GetRelativePath($Directory, $_.FullName).Replace('\', '/') }
    [Array]::Sort($files, [StringComparer]::Ordinal)
    $stream = [IO.File]::Create($Path)
    try
    {
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
        try
        {
            foreach ($file in $files)
            {
                $entry = $zip.CreateEntry("$Prefix/$file", [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = $Time
                $source = [IO.File]::OpenRead((Join-Path $Directory $file))
                $target = $entry.Open()
                try
                {
                    $source.CopyTo($target)
                }
                finally
                {
                    $target.Dispose()
                    $source.Dispose()
                }
            }
        }
        finally
        {
            $zip.Dispose()
        }
    }
    finally
    {
        $stream.Dispose()
    }
}

#
# Toolchain
#

# The x64 developer environment of the newest Visual Studio with C++, unless already in one
if (-not $env:VCToolsVersion)
{
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere))
    {
        throw 'Visual Studio 2022 or its Build Tools with the C++ workload are needed (vswhere.exe not found).'
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs)
    {
        throw 'No Visual Studio with the C++ x64 tools found.'
    }
    # The pinned MSVC toolset and SDK, when installed next to newer ones (CI images keep several)
    $devCmd = '-arch=x64 -host_arch=x64'
    if (-not $Pin -and (Test-Path $pinFile))
    {
        $pinnedVs = Get-Content $pinFile -Raw | ConvertFrom-Json
        $devCmd  += " -vcvars_ver=$($pinnedVs.msvc) -winsdk=$($pinnedVs.sdk)"
    }
    # The developer shell puts the clang bundled with Visual Studio first; the clang on PATH before
    # it (the installed LLVM) stays the one used
    $clang = Get-Command clang -ErrorAction SilentlyContinue
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments $devCmd | Out-Null
    if ($clang)
    {
        $env:PATH = "$(Split-Path $clang.Source);$env:PATH"
    }
}

foreach ($program in 'clang', 'clang++', 'lld-link', 'llvm-objdump', 'glslc', 'cmake', 'ninja', 'git')
{
    if (-not (Get-Command $program -ErrorAction SilentlyContinue))
    {
        throw "$program is not on PATH (docs/building.md, Windows releases)."
    }
}

$toolchain = [ordered]@{
    clang      = ((& clang --version) | Select-Object -First 1).Trim()
    msvc       = $env:VCToolsVersion.Trim()
    sdk        = $env:WindowsSDKVersion.Trim().TrimEnd('\')
    glslc      = ((& glslc --version) | Select-Object -First 1).Trim()
    powershell = "$($PSVersionTable.PSVersion.Major).$($PSVersionTable.PSVersion.Minor)"
}

if ($Pin)
{
    $toolchain | ConvertTo-Json | Set-Content -Path $pinFile -Encoding utf8NoBOM
    Write-Host "Pinned the toolchain in ${pinFile}:"
    Get-Content $pinFile
    exit 0
}
if (-not (Test-Path $pinFile))
{
    throw "No $pinFile yet: run tools\dist\dist.ps1 -Pin once and commit the file."
}
$pinned     = Get-Content $pinFile -Raw | ConvertFrom-Json
$mismatches = @()
foreach ($name in $toolchain.Keys)
{
    if ($pinned.$name -ne $toolchain[$name])
    {
        $mismatches += "  ${name}: pinned '$($pinned.$name)', found '$($toolchain[$name])'"
    }
}
if ($mismatches)
{
    throw "The toolchain differs from $pinFile, so the release would not be reproducible:`n$($mismatches -join "`n")`nInstall the pinned versions, or run -Pin to move the pin and commit it."
}

#
# Build and test
#

$commit = (& git -C $root rev-parse HEAD).Trim()
if (& git -C $root status --porcelain --untracked-files=no)
{
    Write-Warning "Uncommitted changes, the release will not match $commit"
    $commit = "$commit-dirty"
}
$epoch = [long](& git -C $root log -1 --format=%ct).Trim()

Invoke-Native cmake --preset win-clang-dist -B $build
Invoke-Native cmake --build $build
if ($NoTests)
{
    Invoke-Native ctest --test-dir $build --output-on-failure -R '^(shipped_binaries_need_only_windows_dlls|dll_check_rejects_other_dlls)$'
}
else
{
    Invoke-Native ctest --test-dir $build --output-on-failure -j $env:NUMBER_OF_PROCESSORS
}

#
# Package
#

$version = (Select-String -Path (Join-Path $build 'CMakeCache.txt') -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$').Matches[0].Groups[1].Value
$name    = "puppetxr-$version-windows-x64"
$stage   = Join-Path $build 'stage'
$time    = [DateTimeOffset]::FromUnixTimeSeconds($epoch)
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stage, $out
New-Item -ItemType Directory -Force (Join-Path $stage "$name-pdb"), $out | Out-Null
Invoke-Native cmake --install $build --prefix (Join-Path $stage $name)
Copy-Item (Join-Path $build 'bin\puppetxr.pdb'), (Join-Path $build 'bin\puppetxrctl.pdb') (Join-Path $stage "$name-pdb")
foreach ($tree in $name, "$name-pdb")
{
    New-ReproducibleZip -Directory (Join-Path $stage $tree) -Prefix $tree -Path (Join-Path $out "$tree.zip") -Time $time
}

$lines = @("commit: $commit", 'toolchain:')
$lines += $toolchain.Keys | ForEach-Object { "  ${_}: $($toolchain[$_])" }
$lines += 'sha256:'
$lines += Get-ChildItem -Path $out -Filter '*.zip' | Sort-Object Name | ForEach-Object { "  $((Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant())  $($_.Name)" }
$lines | Set-Content -Path (Join-Path $out "$name.buildinfo") -Encoding utf8NoBOM
$lines | Write-Host
Write-Host "Release in $out"
