#!/usr/bin/env pwsh
# Detect Resident Evil 3 Remake installation directory

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Extract Steam library paths from libraryfolders.vdf content. Pure (content in,
# paths out) so it is unit-testable without a real Steam install. Returns an
# empty array for null/empty content: Get-Content -Raw yields $null for an empty
# file and [regex]::Matches throws on null input, so a truncated or empty VDF
# would otherwise crash detection instead of falling through to the known
# folder-name search.
function Get-VdfLibraryPaths {
    param([string]$VdfContent)

    if ([string]::IsNullOrEmpty($VdfContent)) {
        return @()
    }

    $paths = @()
    foreach ($match in [regex]::Matches($VdfContent, '"path"\s+"([^"]+)"')) {
        $path = $match.Groups[1].Value -replace '\\\\', '\'
        if ($path) {
            $paths += $path
        }
    }
    return $paths
}

function Find-RE3Installation {
    # Check environment variable override first
    if ($env:RE3_PATH) {
        $gamePath = $env:RE3_PATH
        if (Test-RE3Installation $gamePath) {
            return $gamePath
        }
        Write-Warning "RE3_PATH is set but path is invalid: $gamePath"
    }

    # Find Steam installation
    $steamPath = $null

    # Try registry (64-bit)
    try {
        $steamPath = (Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -ErrorAction Stop).InstallPath
    } catch { }

    # Try registry (32-bit fallback)
    if (-not $steamPath) {
        try {
            $steamPath = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Valve\Steam" -ErrorAction Stop).InstallPath
        } catch { }
    }

    if (-not $steamPath) {
        return $null
    }

    # Parse libraryfolders.vdf to find all Steam library paths
    $libraryFolders = @($steamPath)
    $vdfPath = Join-Path $steamPath "steamapps\libraryfolders.vdf"

    if (Test-Path $vdfPath) {
        $content = Get-Content $vdfPath -Raw
        foreach ($path in (Get-VdfLibraryPaths $content)) {
            if (Test-Path $path) {
                $libraryFolders += $path
            }
        }
    }

    # Known folder names for RE3 Remake (canonical first, see cameraunlock-core/data/games.json)
    $folderNames = @(
        "RE3",
        "Resident Evil 3"
    )

    # Search each library for RE3
    foreach ($library in $libraryFolders) {
        foreach ($folderName in $folderNames) {
            $gamePath = Join-Path $library "steamapps\common\$folderName"
            if (Test-RE3Installation $gamePath) {
                return $gamePath
            }
        }
    }

    return $null
}

function Test-RE3Installation {
    param([string]$path)

    if (-not (Test-Path $path)) {
        return $false
    }

    $exePath = Join-Path $path "re3.exe"
    return (Test-Path $exePath)
}

# Main. Skipped when the script is dot-sourced (e.g. by tests) so loading the
# functions does not trigger registry lookups or exit the host.
if ($MyInvocation.InvocationName -ne '.') {
    $gamePath = Find-RE3Installation

    if ($gamePath) {
        Write-Output $gamePath
        exit 0
    } else {
        Write-Host ""
        Write-Host "========================================" -ForegroundColor Red
        Write-Host "  ERROR: Resident Evil 3 not found" -ForegroundColor Red
        Write-Host "========================================" -ForegroundColor Red
        Write-Host ""
        Write-Host "To fix this:" -ForegroundColor Yellow
        Write-Host "  1. Find your RE3 installation folder" -ForegroundColor White
        Write-Host "  2. Set the environment variable:" -ForegroundColor White
        Write-Host "     `$env:RE3_PATH = 'C:\path\to\RE3'" -ForegroundColor Green
        Write-Host "  3. Run deploy again:" -ForegroundColor White
        Write-Host "     pixi run deploy" -ForegroundColor Green
        Write-Host ""
        exit 1
    }
}
