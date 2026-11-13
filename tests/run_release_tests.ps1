#Requires -Version 5.1
# NOTE: Keep this script ASCII-only. Windows PowerShell 5.1 parses .ps1 files
#       without a BOM using the ANSI code page, so non-ASCII comments can break
#       parsing.
#
# tests/run_release_tests.ps1 -- clean offline verification entry point for GakuMotion.
#
#   1. Build and run the deterministic offline unit tests (tests/test_refresh_core.cpp)
#   2. Verify stage-load detour forwarding, metadata and shutdown
#   3. Syntax-check the ImGui overlay skeleton (tests/gui_syntax_check.cpp)
#
# Toolchain policy:
#   Visual Studio 2022+ MSVC (/std:c++20 /utf-8)
#
# Usage:
#   powershell -NoProfile -File tests\run_tests.ps1
#
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$ImGuiInclude = '',
    [string]$KineTextureInclude = '',
    [string]$VisualStudioPath = '',
    [string]$WindowsSdkRoot = '',
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
        $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
    }
}

if ([string]::IsNullOrWhiteSpace($ImGuiInclude)) {
    $ImGuiInclude = Join-Path $RepoRoot 'deps\imgui'
}

if ([string]::IsNullOrWhiteSpace($KineTextureInclude)) {
    $KineTextureInclude = Join-Path $RepoRoot 'deps\KineTexture\src'
}

$srcDir = Join-Path $RepoRoot 'src'
$incDir = Join-Path $RepoRoot 'include'
$buildDir = if ($OutputRoot) { Join-Path $OutputRoot 'native-tests' } else { Join-Path $RepoRoot 'build\tests' }
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

$testExe = Join-Path $buildDir 'test_core.exe'
$testSource = Join-Path $PSScriptRoot 'test_refresh_core.cpp'
$syntaxSource = Join-Path $PSScriptRoot 'gui_syntax_check.cpp'

# Locate Visual Studio with MSVC x64
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio with the 'Desktop development with C++' workload."
}
$vsPath = $VisualStudioPath
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    $vsPath = & $vswhere -latest -prerelease -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
}
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    throw "No Visual Studio installation with the MSVC x64 toolset was found."
}

# Resolve MSVC toolset
$msvcRoot = Join-Path $vsPath 'VC\Tools\MSVC'
$msvcVer  = Get-ChildItem -Path $msvcRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name | Select-Object -Last 1
if (-not $msvcVer) { throw "No MSVC toolset found under: $msvcRoot" }
$msvcTools = $msvcVer.FullName

# Resolve Windows 10/11 SDK
$sdkRoot = $null
foreach ($candidate in @($WindowsSdkRoot, 'D:\Windows Kits\10',
                        "${env:ProgramFiles(x86)}\Windows Kits\10",
                        "${env:ProgramFiles}\Windows Kits\10")) {
    if ($candidate -and (Test-Path (Join-Path $candidate 'Include'))) { $sdkRoot = $candidate; break }
}
if (-not $sdkRoot) { throw "No Windows 10/11 SDK found." }
$sdkVer = Get-ChildItem -Path (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name | Select-Object -Last 1
if (-not $sdkVer) { throw "No SDK version found under: $sdkRoot\Include" }
$sdkVersion = $sdkVer.Name

$incDirs = @(
    "$msvcTools\include",
    "$sdkRoot\Include\$sdkVersion\ucrt",
    "$sdkRoot\Include\$sdkVersion\um",
    "$sdkRoot\Include\$sdkVersion\shared",
    "$sdkRoot\Include\$sdkVersion\winrt",
    "$sdkRoot\Include\$sdkVersion\cppwinrt"
)
$libDirs = @(
    "$msvcTools\lib\x64",
    "$sdkRoot\Lib\$sdkVersion\ucrt\x64",
    "$sdkRoot\Lib\$sdkVersion\um\x64"
)
Set-Item -Path env:INCLUDE -Value ($incDirs -join ';')
Set-Item -Path env:LIB     -Value ($libDirs -join ';')
Set-Item -Path env:PATH    -Value "$msvcTools\bin\Hostx64\x64;$env:PATH"

$compiler = Join-Path $msvcTools 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $compiler)) { throw "cl.exe not found: $compiler" }

Write-Host "  MSVC : $msvcTools" -ForegroundColor DarkGray
Write-Host "  SDK  : $sdkRoot ($sdkVersion)" -ForegroundColor DarkGray

$objDir = Join-Path $buildDir 'obj-msvc'
if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir -Force | Out-Null }

$testObj = Join-Path $objDir 'test_core.obj'
$testArgs = @('-nologo', '/std:c++20', '/utf-8', '/O1', '/W3', '/EHsc', '/D_CRT_SECURE_NO_WARNINGS',
              "-I$srcDir", "-I$incDir", "-I$KineTextureInclude", "-I$PSScriptRoot",
              $testSource, "-Fe:$testExe", "-Fo:$testObj")

Write-Host "=== [1/3] Build and run offline unit tests (MSVC) ===" -ForegroundColor Cyan
& $compiler @testArgs
if ($LASTEXITCODE -ne 0) { throw "Unit test build failed (exit=$LASTEXITCODE)" }
Write-Host "  built: $testExe" -ForegroundColor Green

& $testExe $RepoRoot
$testExit = $LASTEXITCODE
if ($testExit -ne 0) {
    Write-Host "Unit tests FAILED (exit=$testExit)" -ForegroundColor Red
    exit $testExit
}
Write-Host "  all unit tests passed" -ForegroundColor Green

# Exercise assignment edge cases independently of the Core integration suite.
$ensembleExe = Join-Path $buildDir 'test_ensemble.exe'
$ensembleArgs = @('-nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/W4',
    (Join-Path $PSScriptRoot 'test_ensemble.cpp'),
    "/Fe:$ensembleExe", "/Fo:$(Join-Path $objDir 'test_ensemble.obj')")
& $compiler @ensembleArgs
if ($LASTEXITCODE -ne 0) { throw "Ensemble assignment test build failed (exit=$LASTEXITCODE)" }
& $ensembleExe
if ($LASTEXITCODE -ne 0) { throw "Ensemble assignment tests failed (exit=$LASTEXITCODE)" }
Write-Host "  ensemble assignment and timing checks passed" -ForegroundColor Green

$mikuRunnerExe = Join-Path $buildDir 'miku_fit_runner.exe'
$mikuRunnerArgs = @('-nologo', '/std:c++20', '/utf-8', '/O2', '/EHsc', '/MT', '/W4',
    "/I$srcDir", "/I$incDir", (Join-Path $PSScriptRoot 'miku_fit_runner.cpp'),
    "/Fe:$mikuRunnerExe", "/Fo:$(Join-Path $objDir 'miku_fit_runner.obj')")
& $compiler @mikuRunnerArgs
if ($LASTEXITCODE -ne 0) { throw "Miku fit JSONL runner build failed (exit=$LASTEXITCODE)" }

Write-Host "=== [2/3] Stage-load detour forwarding and metadata ===" -ForegroundColor Cyan
$stageLoadExe = Join-Path $buildDir 'test_stage_load_runtime.exe'
$stageLoadArgs = @('-nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/W4',
    "/I$srcDir", "/I$incDir", (Join-Path $PSScriptRoot 'test_stage_load_runtime.cpp'),
    "/Fe:$stageLoadExe", "/Fo:$(Join-Path $objDir 'test_stage_load_runtime.obj')")
& $compiler @stageLoadArgs
if ($LASTEXITCODE -ne 0) { throw "Stage-load detour test build failed (exit=$LASTEXITCODE)" }
& $stageLoadExe
if ($LASTEXITCODE -ne 0) { throw "Stage-load detour tests failed (exit=$LASTEXITCODE)" }

Write-Host "=== [3/3] ImGui overlay skeleton syntax check ===" -ForegroundColor Cyan
if (Test-Path (Join-Path $ImGuiInclude 'imgui.h')) {
    $syntaxArgs = @('-nologo', '/std:c++20', '/utf-8', '/Zs', '/W3', '/EHsc', '/DGKMOTION_ENABLE_IMGUI',
                    "/I$srcDir", "/I$incDir", "/I$KineTextureInclude", "/I$ImGuiInclude", $syntaxSource)
    & $compiler @syntaxArgs
    if ($LASTEXITCODE -ne 0) { throw "ImGui overlay syntax check failed (exit=$LASTEXITCODE)" }
    Write-Host "  OK: GkGuiOverlay.hpp compiles cleanly with vendored ImGui" -ForegroundColor Green
} else {
    Write-Host "  SKIP: ImGui include dir not found: $ImGuiInclude" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "All GakuMotion offline verifications PASSED successfully." -ForegroundColor Green
exit 0
