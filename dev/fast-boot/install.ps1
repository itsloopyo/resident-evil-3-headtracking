#!/usr/bin/env pwsh
# Deploys the fast-boot Lua autorun script to the RE3 install.
# Usage: pwsh ./install.ps1 [-GamePath <path>] [-Uninstall]

param(
    [string]$GamePath = "C:\Program Files (x86)\Steam\steamapps\common\RE3",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $GamePath)) {
    throw "Game path not found: $GamePath"
}

$autorunDir = Join-Path $GamePath "reframework\autorun"
if (-not (Test-Path $autorunDir)) {
    throw "REFramework autorun folder not found: $autorunDir - install REFramework first"
}

$target = Join-Path $autorunDir "re3_fast_boot.lua"

if ($Uninstall) {
    if (Test-Path $target) {
        Remove-Item -LiteralPath $target -Force
        Write-Host "Removed: $target"
    } else {
        Write-Host "Not installed (nothing to remove)."
    }
    return
}

$source = Join-Path $PSScriptRoot "re3_fast_boot.lua"
if (-not (Test-Path $source)) {
    throw "Source script missing: $source"
}

Copy-Item -LiteralPath $source -Destination $target -Force
Write-Host "Installed: $target"
