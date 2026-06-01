#!/usr/bin/env pwsh
#Requires -Version 5.1
# Automated release workflow for RE3 Head Tracking.
#
# Steps:
#   1. Validate semver argument
#   2. Verify on `main` branch with a clean working tree
#   3. Verify the v<version> tag does not already exist
#   4. Update version in constants.h (canonical, compiled into the DLL),
#      manifest.json, CMakeLists.txt, and MOD_VERSION in install.cmd - all
#      kept in lockstep so the built binary logs the released version.
#   5. Run `pixi run build` (Release config). Abort on failure.
#   6. Generate CHANGELOG.md from commits via ReleaseWorkflow.psm1
#   7. Commit "Release v<version>" including version + changelog
#   8. Annotated tag v<version> and push commits + tag (CI release.yml fires)
#
# Headless: no interactive prompts of any kind. The pre-flight checks gate
# destructive steps; if they pass, the user has chosen to release.

param(
    [Parameter(Position=0)]
    [string]$Version = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir    = Split-Path -Parent $scriptDir
$manifestPath  = Join-Path $projectDir "manifest.json"
$cmakePath     = Join-Path $projectDir "CMakeLists.txt"
$constantsPath = Join-Path $projectDir "src\core\constants.h"

$module = Join-Path $projectDir "cameraunlock-core\powershell\ReleaseWorkflow.psm1"
if (-not (Test-Path $module)) {
    throw "ReleaseWorkflow.psm1 not found at $module. Run 'pixi run sync' to populate the cameraunlock-core submodule."
}
Import-Module $module -Force

function Get-CurrentVersion {
    $json = Get-Content $manifestPath -Raw | ConvertFrom-Json
    return $json.version
}

function Set-Version {
    param([string]$NewVersion)
    $json = Get-Content $manifestPath -Raw | ConvertFrom-Json
    $json.version = $NewVersion
    $json | ConvertTo-Json -Depth 10 | Set-Content $manifestPath -NoNewline
}

Write-Host "=== RE3 Head Tracking Release ===" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path $manifestPath)) {
    throw "manifest.json not found at $manifestPath."
}

$currentVersion = Get-CurrentVersion

if ([string]::IsNullOrWhiteSpace($Version)) {
    Write-Host "Current version: " -NoNewline -ForegroundColor Yellow
    Write-Host $currentVersion -ForegroundColor White
    Write-Host ""
    Write-Host "Usage: pixi run release <major|minor|patch|nightly|X.Y.Z>" -ForegroundColor Yellow
    exit 0
}

if ($Version -eq 'nightly') {
    & (Join-Path $PSScriptRoot 'release-nightly.ps1')
    exit $LASTEXITCODE
}

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Host "Error: Invalid version format '$Version'. Use semantic versioning X.Y.Z." -ForegroundColor Red
    exit 1
}

$tagName = "v$Version"

$currentBranch = git rev-parse --abbrev-ref HEAD
if ($currentBranch -ne "main") {
    Write-Host "Error: Must be on 'main' branch (currently on '$currentBranch')." -ForegroundColor Red
    exit 1
}

$status = git status --porcelain
if ($status) {
    Write-Host "Error: Working directory has uncommitted changes." -ForegroundColor Red
    exit 1
}

$existingTag = git tag -l $tagName
if ($existingTag) {
    Write-Host "Error: Tag '$tagName' already exists." -ForegroundColor Red
    exit 1
}

Write-Host "Current version: $currentVersion" -ForegroundColor Gray
Write-Host "New version:     $Version" -ForegroundColor Green
Write-Host ""

Write-Host "Updating version to $Version..." -ForegroundColor Cyan
Set-Version $Version

# constants.h is the canonical version the DLL compiles in and logs at
# runtime. Bump it before the build below so the released binary reports
# the released version, not 0.0.0.
if (-not (Test-Path $constantsPath)) {
    throw "constants.h not found at $constantsPath."
}
(Get-Content $constantsPath -Raw) -replace 'RE3HT_VERSION\s*=\s*"[^"]*"', "RE3HT_VERSION = `"$Version`"" |
    Set-Content $constantsPath -NoNewline

# Keep the CMake project version in lockstep with the canonical source.
if (-not (Test-Path $cmakePath)) {
    throw "CMakeLists.txt not found at $cmakePath."
}
(Get-Content $cmakePath -Raw) -replace '(project\(RE3HeadTracking VERSION )\d+\.\d+\.\d+', "`${1}$Version" |
    Set-Content $cmakePath -NoNewline

# Mirror the version into install.cmd's MOD_VERSION so the installer log
# matches the release tag the user just downloaded.
$installCmdPath = Join-Path $scriptDir "install.cmd"
if (Test-Path $installCmdPath) {
    (Get-Content $installCmdPath -Raw) -replace 'set "MOD_VERSION=.*?"', "set `"MOD_VERSION=$Version`"" |
        Set-Content $installCmdPath -NoNewline
}

Write-Host "Building Release configuration..." -ForegroundColor Cyan
pixi run build
if ($LASTEXITCODE -ne 0) {
    Write-Host "Error: Build failed; aborting release." -ForegroundColor Red
    exit 1
}

Write-Host "Generating CHANGELOG..." -ForegroundColor Cyan
$changelogPath = Join-Path $projectDir "CHANGELOG.md"
$hasExistingTags = git tag -l 2>$null
if (-not $hasExistingTags) {
    $date = Get-Date -Format 'yyyy-MM-dd'
    $firstEntry = "# Changelog`n`n## [$Version] - $date`n`nFirst release.`n"
    Set-Content $changelogPath $firstEntry
} else {
    $changelogArgs = @{
        ChangelogPath = $changelogPath
        Version       = $Version
        ArtifactPaths = @("src/", "cameraunlock-core/", "scripts/install.cmd", "scripts/uninstall.cmd")
    }
    New-ChangelogFromCommits @changelogArgs
}

Write-Host "Committing version change..." -ForegroundColor Cyan
$filesToStage = @($manifestPath, $cmakePath, $constantsPath, $changelogPath)
if (Test-Path $installCmdPath) { $filesToStage += $installCmdPath }
git add -- $filesToStage
git commit -m "Release v$Version"
if ($LASTEXITCODE -ne 0) {
    throw "git commit failed"
}

Write-Host "Creating annotated tag $tagName..." -ForegroundColor Cyan
git tag -a $tagName -m "Release $tagName"
if ($LASTEXITCODE -ne 0) {
    throw "git tag failed"
}

Write-Host "Pushing to GitHub..." -ForegroundColor Cyan
git push origin main
if ($LASTEXITCODE -ne 0) {
    throw "git push origin main failed"
}
git push origin $tagName
if ($LASTEXITCODE -ne 0) {
    throw "git push origin $tagName failed"
}

Write-Host ""
Write-Host "Release $tagName initiated. CI release workflow will publish artifacts." -ForegroundColor Green
