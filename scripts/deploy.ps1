#!/usr/bin/env pwsh
#Requires -Version 5.1
# Deploy a local build of RE3HeadTracking.dll to a detected game install
# for dev testing. End users use scripts/install.cmd from the release ZIP.
#
# Game detection order (matches install.cmd via shared GamePathDetection):
#   1. RE3_PATH env var
#   2. Steam app manifest (app id 952060)
#   3. Steam library folder name (RE3)
#
# REFramework is sourced from the committed vendor/reframework/RE3.zip;
# refresh it via `pixi run update-deps`. No network at deploy time.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

$configuration = if ($args[0]) { $args[0] } else { "Debug" }

Write-Host "Deploying $configuration build to Resident Evil 3..." -ForegroundColor Cyan

$module = Join-Path $projectDir 'cameraunlock-core/powershell/GamePathDetection.psm1'
if (-not (Test-Path $module)) {
    throw "GamePathDetection.psm1 not found at $module. Run 'pixi run sync' to populate the cameraunlock-core submodule."
}
Import-Module $module -Force

$gamePath = Find-GamePath -GameId 'resident-evil-3'
if (-not $gamePath) {
    Write-Host ""
    Write-Host "ERROR: Resident Evil 3 not found." -ForegroundColor Red
    Write-Host "Set the RE3_PATH environment variable to your install directory and retry:" -ForegroundColor Yellow
    Write-Host "  `$env:RE3_PATH = 'C:\path\to\RE3'" -ForegroundColor Green
    exit 1
}

Write-Host "Game directory: $gamePath" -ForegroundColor Gray

# Install REFramework from the vendored copy if not already present.
$reframeworkDll = Join-Path $gamePath "dinput8.dll"
if (-not (Test-Path $reframeworkDll)) {
    $vendorZip = Join-Path $projectDir "vendor\reframework\RE3.zip"
    if (-not (Test-Path $vendorZip)) {
        throw "REFramework not installed and vendored copy missing at $vendorZip. Run 'pixi run update-deps' to fetch it, then retry."
    }
    Write-Host "REFramework not found. Extracting bundled copy..." -ForegroundColor Yellow
    Expand-Archive -Path $vendorZip -DestinationPath $gamePath -Force
    if (-not (Test-Path $reframeworkDll)) {
        throw "REFramework install failed: dinput8.dll not present after extraction."
    }
    Write-Host "  REFramework installed from vendor/reframework/RE3.zip." -ForegroundColor Green
} else {
    Write-Host "REFramework present, skipping loader install." -ForegroundColor Gray
}

$pluginsDir = Join-Path $gamePath "reframework\plugins"
if (-not (Test-Path $pluginsDir)) {
    New-Item -ItemType Directory -Path $pluginsDir -Force | Out-Null
    Write-Host "  Created: reframework/plugins/" -ForegroundColor Green
}

$sourceDll = Join-Path $projectDir "bin\$configuration\RE3HeadTracking.dll"
$sourceIni = Join-Path $projectDir "HeadTracking.ini"

if (-not (Test-Path $sourceDll)) {
    Write-Error "Build artifact not found: $sourceDll"
    Write-Host "Run 'pixi run build' first." -ForegroundColor Yellow
    exit 1
}

$targetDll = Join-Path $pluginsDir "RE3HeadTracking.dll"
$targetIni = Join-Path $pluginsDir "HeadTracking.ini"

if (Test-Path $targetDll) {
    Copy-Item $targetDll "$targetDll.bak" -Force
    Write-Host "  Backed up existing DLL" -ForegroundColor Gray
}

Copy-Item $sourceDll $targetDll -Force
Write-Host "  Copied: RE3HeadTracking.dll" -ForegroundColor Green

# Preserve the user's INI on redeploy; only seed the default if missing.
if (-not (Test-Path $targetIni)) {
    if (Test-Path $sourceIni) {
        Copy-Item $sourceIni $targetIni -Force
        Write-Host "  Copied: HeadTracking.ini (default config)" -ForegroundColor Green
    }
} else {
    Write-Host "  Skipped: HeadTracking.ini (preserving existing config)" -ForegroundColor Gray
}

Write-Host ""
Write-Host "Deployment complete!" -ForegroundColor Green
Write-Host "Files deployed to: $pluginsDir" -ForegroundColor Cyan
