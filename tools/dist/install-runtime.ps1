# Makes this puppetxr the active OpenXR runtime of 64-bit apps, or with -Uninstall gives the role
# back to the runtime that had it. Needs an administrator: the loader reads
# HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime. Runs on Windows PowerShell 5.1 and PowerShell 7:
#   powershell -ExecutionPolicy Bypass -File install-runtime.ps1 [-Uninstall]

param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'

$khronos  = 'HKLM:\SOFTWARE\Khronos\OpenXR\1'
$saved    = 'HKLM:\SOFTWARE\puppetxr'
$manifest = Join-Path $PSScriptRoot 'share\openxr\1\puppetxr.json'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
{
    throw 'Run this from an administrator PowerShell: the active runtime is a machine-wide setting.'
}

$current = (Get-ItemProperty -Path $khronos -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime

if ($Uninstall)
{
    if ($current -ne $manifest)
    {
        Write-Host "puppetxr is not the active runtime ($current); nothing changed."
        exit 0
    }
    $previous = (Get-ItemProperty -Path $saved -Name PreviousActiveRuntime -ErrorAction SilentlyContinue).PreviousActiveRuntime
    if ($previous)
    {
        Set-ItemProperty -Path $khronos -Name ActiveRuntime -Value $previous
        Write-Host "Active OpenXR runtime: $previous"
    }
    else
    {
        Remove-ItemProperty -Path $khronos -Name ActiveRuntime
        Write-Host 'No OpenXR runtime is active.'
    }
    Remove-Item -Path $saved -Recurse -ErrorAction SilentlyContinue
    exit 0
}

if (-not (Test-Path $manifest))
{
    throw "No manifest at ${manifest}; run the script from the unpacked release."
}
if ($current -eq $manifest)
{
    Write-Host "puppetxr is already the active runtime: $manifest"
    exit 0
}
# Another puppetxr release is never remembered, so installing one over another still gives the role
# back to the runtime from before
if ($current -and -not ($current -like '*\share\openxr\1\puppetxr.json'))
{
    if (-not (Test-Path $saved))
    {
        New-Item -Path $saved | Out-Null
    }
    Set-ItemProperty -Path $saved -Name PreviousActiveRuntime -Value $current
}
# New-Item -Force would recreate an existing key without its values
if (-not (Test-Path $khronos))
{
    New-Item -Path $khronos -Force | Out-Null
}
Set-ItemProperty -Path $khronos -Name ActiveRuntime -Value $manifest
Write-Host "Active OpenXR runtime: $manifest"
