#Requires -Version 5.1
# NOTE: Keep this script ASCII-only. Windows PowerShell 5.1 parses .ps1 files
#       without a BOM using the ANSI code page, so non-ASCII comments can break
#       parsing.
#
# tools/check_loader_contracts.ps1 -- verify SHA256 parity of vendored
# GakuLoader public SDK contract headers against the sibling GakuLoader checkout.
#
#   include/GakuPlugin.h
#   include/GakuPluginMetadata.h
#   include/GakuPaths.h
#   include/GakuPaths.hpp
#   include/GakuWebPanel.h
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_loader_contracts.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_loader_contracts.ps1 -LoaderIncludeRoot D:\path\to\GakuLoader\dev\include
#
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$LoaderIncludeRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
        $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
    }
}

if ([string]::IsNullOrWhiteSpace($LoaderIncludeRoot)) {
    $LoaderIncludeRoot = [IO.Path]::GetFullPath((Join-Path $RepoRoot '..\..\GakuLoader\dev\include'))
}

Write-Host "GakuMotion : $RepoRoot"
Write-Host "GakuLoader : $LoaderIncludeRoot"

if (-not (Test-Path -LiteralPath $LoaderIncludeRoot)) {
    Write-Host "  SKIP: GakuLoader include directory not found; nothing to compare against." -ForegroundColor Yellow
    exit 0
}

$headers = @(
    'GakuPlugin.h',
    'GakuPluginMetadata.h',
    'GakuPaths.h',
    'GakuPaths.hpp',
    'GakuWebPanel.h'
)

$failures = 0
foreach ($header in $headers) {
    $mine = Join-Path (Join-Path $RepoRoot 'include') $header
    $theirs = Join-Path $LoaderIncludeRoot $header
    if (-not (Test-Path -LiteralPath $mine -PathType Leaf)) {
        Write-Host "  MISSING (GakuMotion): include\$header" -ForegroundColor Red
        $failures++
        continue
    }
    if (-not (Test-Path -LiteralPath $theirs -PathType Leaf)) {
        Write-Host "  MISSING (GakuLoader): $header" -ForegroundColor Red
        $failures++
        continue
    }
    $left = (Get-FileHash -LiteralPath $mine -Algorithm SHA256).Hash
    $right = (Get-FileHash -LiteralPath $theirs -Algorithm SHA256).Hash
    if ($left -ne $right) {
        Write-Host "  DRIFT: $header" -ForegroundColor Red
        Write-Host "    GakuMotion include\$header  $left"
        Write-Host "    GakuLoader $header          $right"
        $failures++
        continue
    }
    Write-Host ("  OK: {0} ({1})" -f $header, $left.Substring(0, 16)) -ForegroundColor Green
}

if ($failures -ne 0) {
    Write-Host "GakuLoader contract header parity FAILED ($failures)." -ForegroundColor Red
    exit 1
}

Write-Host 'GakuLoader contract header parity passed.'
exit 0
