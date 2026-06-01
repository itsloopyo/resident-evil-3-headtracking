# Regression tests for scripts/detect-game.ps1.
# Guards the fix where the validator checked for re2.exe (RE2 Remake) instead
# of re3.exe, which could detect an RE2 install and report it as RE3.
#
# Run with: powershell -NoProfile -Command "Invoke-Pester -Path tests\detect-game.Tests.ps1"

$here       = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path $here '..\scripts\detect-game.ps1'

# Dot-source: the script's main block is guarded so this only loads functions.
. $scriptPath

function New-TempGameDir {
    param([string]$ExeName)
    $dir = Join-Path $env:TEMP ("re3test_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    if ($ExeName) {
        New-Item -ItemType File -Path (Join-Path $dir $ExeName) -Force | Out-Null
    }
    return $dir
}

Describe 'Test-RE3Installation' {

    It 'accepts a directory containing re3.exe' {
        $dir = New-TempGameDir -ExeName 're3.exe'
        try {
            Test-RE3Installation $dir | Should Be $true
        } finally {
            Remove-Item $dir -Recurse -Force
        }
    }

    It 'rejects a directory containing only re2.exe (wrong game)' {
        $dir = New-TempGameDir -ExeName 're2.exe'
        try {
            Test-RE3Installation $dir | Should Be $false
        } finally {
            Remove-Item $dir -Recurse -Force
        }
    }

    It 'rejects a directory with no game executable' {
        $dir = New-TempGameDir -ExeName $null
        try {
            Test-RE3Installation $dir | Should Be $false
        } finally {
            Remove-Item $dir -Recurse -Force
        }
    }

    It 'rejects a path that does not exist' {
        $missing = Join-Path $env:TEMP ([guid]::NewGuid().ToString("N"))
        Test-RE3Installation $missing | Should Be $false
    }
}

Describe 'Get-VdfLibraryPaths' {

    It 'extracts every library path entry' {
        $vdf = @'
"libraryfolders"
{
    "0"  { "path"   "C:\\Program Files (x86)\\Steam" }
    "1"  { "path"   "D:\\SteamLibrary" }
}
'@
        $result = @(Get-VdfLibraryPaths $vdf)
        $result.Count          | Should Be 2
        $result[0]             | Should Be 'C:\Program Files (x86)\Steam'
        $result[1]             | Should Be 'D:\SteamLibrary'
    }

    It 'unescapes the VDF double-backslash path separator' {
        $result = @(Get-VdfLibraryPaths '"path"   "D:\\Games\\Steam"')
        $result[0] | Should Be 'D:\Games\Steam'
    }

    It 'returns an empty array for empty content (empty libraryfolders.vdf)' {
        # Guards against the crash where Get-Content -Raw returns $null for an
        # empty file and [regex]::Matches then throws on null input.
        @(Get-VdfLibraryPaths '').Count   | Should Be 0
        @(Get-VdfLibraryPaths $null).Count | Should Be 0
    }

    It 'returns an empty array when no path entries are present' {
        @(Get-VdfLibraryPaths '"libraryfolders" { "contentstatsid" "123" }').Count | Should Be 0
    }
}
