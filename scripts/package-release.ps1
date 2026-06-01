#!/usr/bin/env pwsh
#Requires -Version 5.1
# Stage and zip the two release archives for RE3 Head Tracking:
#
#   release/RE3HeadTracking-v<version>-installer.zip   GitHub Releases artifact
#   release/RE3HeadTracking-v<version>-nexus.zip       Nexus extract-to-game-folder
#
# Offline by design: consumes the committed vendor/reframework/RE3.zip
# verbatim. Bumping the vendored loader is a manual `pixi run update-deps`
# step the dev runs before tagging a release.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference    = 'SilentlyContinue'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

Import-Module (Join-Path $projectDir "cameraunlock-core\powershell\ReleaseWorkflow.psm1") -Force

$manifestPath = Join-Path $projectDir "manifest.json"
if (-not (Test-Path $manifestPath)) {
    throw "manifest.json not found at $manifestPath. Cannot resolve mod version."
}
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$version  = $manifest.version

Write-Host "=== RE3 Head Tracking - Package Release ===" -ForegroundColor Magenta
Write-Host ""
Write-Host "Version: $version" -ForegroundColor Cyan
Write-Host ""

$releaseDir = Join-Path $projectDir "release"
if (-not (Test-Path $releaseDir)) {
    New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
}

# Required inputs. Fail fast if any are missing.
$dllPath = Join-Path $projectDir "bin\Release\RE3HeadTracking.dll"
if (-not (Test-Path $dllPath)) {
    throw "RE3HeadTracking.dll not found at: $dllPath. Run 'pixi run build' first."
}

$iniPath = Join-Path $projectDir "HeadTracking.ini"
if (-not (Test-Path $iniPath)) {
    throw "HeadTracking.ini not found at: $iniPath."
}

$scriptsDir = Join-Path $projectDir "scripts"
foreach ($script in @("install.cmd", "uninstall.cmd")) {
    $scriptPath = Join-Path $scriptsDir $script
    if (-not (Test-Path $scriptPath)) {
        throw "Required script not found: $scriptPath"
    }
}

$vendorDir = Join-Path $projectDir "vendor\reframework"
$vendorZip = Join-Path $vendorDir "RE3.zip"
if (-not (Test-Path $vendorZip)) {
    throw "Vendored REFramework not found at: $vendorZip. Run 'pixi run update-deps' before packaging."
}

# --- Installer (GitHub Releases) ZIP ---
Write-Host "--- Installer ZIP (GitHub Releases) ---" -ForegroundColor Yellow
Write-Host ""

$ghStagingDir = Join-Path $releaseDir "staging-installer"
if (Test-Path $ghStagingDir) { Remove-Item -Recurse -Force $ghStagingDir }
New-Item -ItemType Directory -Path $ghStagingDir -Force | Out-Null

foreach ($script in @("install.cmd", "uninstall.cmd")) {
    Copy-Item (Join-Path $scriptsDir $script) -Destination $ghStagingDir -Force
    Write-Host "  $script" -ForegroundColor Green
}

# install.cmd / uninstall.cmd resolve the game via shared/find-game.ps1.
# Bundle that shim alongside them so the release ZIP is self-contained.
Copy-SharedBundle -StagingDir $ghStagingDir

$pluginsDir = Join-Path $ghStagingDir "plugins"
New-Item -ItemType Directory -Path $pluginsDir -Force | Out-Null

Copy-Item $dllPath -Destination $pluginsDir -Force
Write-Host "  plugins/RE3HeadTracking.dll" -ForegroundColor Green

Copy-Item $iniPath -Destination $pluginsDir -Force
Write-Host "  plugins/HeadTracking.ini" -ForegroundColor Green

# Vendored loader: zip + LICENSE + README only. install.cmd extracts the zip
# directly; no fetch-latest.ps1 ships in the release (offline doctrine).
$ghVendorDir = Join-Path $ghStagingDir "vendor\reframework"
New-Item -ItemType Directory -Path $ghVendorDir -Force | Out-Null
Copy-Item $vendorZip -Destination $ghVendorDir -Force
Write-Host "  vendor/reframework/RE3.zip" -ForegroundColor Green
foreach ($vf in @("LICENSE", "README.md")) {
    $src = Join-Path $vendorDir $vf
    if (Test-Path $src) {
        Copy-Item $src -Destination $ghVendorDir -Force
        Write-Host "  vendor/reframework/$vf" -ForegroundColor Green
    } else {
        Write-Warning "vendor/reframework/$vf missing; release will ship without it"
    }
}

$docFiles = @("README.md", "LICENSE", "CHANGELOG.md", "THIRD-PARTY-NOTICES.md")
foreach ($doc in $docFiles) {
    $docPath = Join-Path $projectDir $doc
    if (Test-Path $docPath) {
        Copy-Item $docPath -Destination $ghStagingDir -Force
        Write-Host "  $doc" -ForegroundColor Green
    }
}

$ghZipName = "RE3HeadTracking-v$version-installer.zip"
$ghZipPath = Join-Path $releaseDir $ghZipName
if (Test-Path $ghZipPath) { Remove-Item $ghZipPath -Force }

Write-Host ""
Write-Host "Creating Installer ZIP..." -ForegroundColor Cyan

Push-Location $ghStagingDir
try {
    Compress-Archive -Path ".\*" -DestinationPath $ghZipPath -Force
} finally {
    Pop-Location
}
Remove-Item -Recurse -Force $ghStagingDir

$ghZipSize = (Get-Item $ghZipPath).Length / 1KB
Write-Host ("  $ghZipPath ({0:N1} KB)" -f $ghZipSize) -ForegroundColor Green

# --- Nexus ZIP (extract-to-game-folder, deploy subtree only) ---
Write-Host ""
Write-Host "--- Nexus ZIP ---" -ForegroundColor Yellow
Write-Host ""

$nexusStagingDir = Join-Path $releaseDir "staging-nexus"
if (Test-Path $nexusStagingDir) { Remove-Item -Recurse -Force $nexusStagingDir }

$nexusPluginsDir = Join-Path $nexusStagingDir "reframework\plugins"
New-Item -ItemType Directory -Path $nexusPluginsDir -Force | Out-Null

Copy-Item $dllPath -Destination $nexusPluginsDir -Force
Write-Host "  reframework/plugins/RE3HeadTracking.dll" -ForegroundColor Green

Copy-Item $iniPath -Destination $nexusPluginsDir -Force
Write-Host "  reframework/plugins/HeadTracking.ini" -ForegroundColor Green

$nexusZipName = "RE3HeadTracking-v$version-nexus.zip"
$nexusZipPath = Join-Path $releaseDir $nexusZipName
if (Test-Path $nexusZipPath) { Remove-Item $nexusZipPath -Force }

Write-Host ""
Write-Host "Creating Nexus ZIP..." -ForegroundColor Cyan

Push-Location $nexusStagingDir
try {
    Compress-Archive -Path ".\*" -DestinationPath $nexusZipPath -Force
} finally {
    Pop-Location
}
Remove-Item -Recurse -Force $nexusStagingDir

$nexusZipSize = (Get-Item $nexusZipPath).Length / 1KB
Write-Host ("  $nexusZipPath ({0:N1} KB)" -f $nexusZipSize) -ForegroundColor Green

Write-Host ""
Write-Host "=== Package Complete ===" -ForegroundColor Magenta
Write-Host ""
Write-Host ("Installer:  $ghZipPath ({0:N1} KB)" -f $ghZipSize) -ForegroundColor Green
Write-Host ("Nexus:      $nexusZipPath ({0:N1} KB)" -f $nexusZipSize) -ForegroundColor Green

Write-Output $ghZipPath
Write-Output $nexusZipPath
