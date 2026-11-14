#Requires -Version 5.1
# NOTE: Keep this script ASCII-only. Windows PowerShell 5.1 parses .ps1 files
#       without a BOM using the ANSI code page, so non-ASCII comments can break
#       parsing.
#
# tools/check_contract_parity.ps1 -- prove the two provider/consumer copies of
# the facial-fit contract are byte-identical.
#
#   include/GakuCharaAdapter.h             <- the character adapter contract
#   include/RinhaFacialFit.h                <- the facial fit contract
#   tools/vmd2gkmotion/miku_raw_eye_keys.json <- the importer's key projection
#
# GakuMotion cannot reference RinhaReplacement (separate plugins, separate
# repos), so these artifacts exist twice. A silent edit on either side would
# produce a DLL that reads the wrong offsets or emits key names the published
# table does not have, so the copies are compared instead of trusted.
#
# Usage:
#   powershell -NoProfile -File tools\check_contract_parity.ps1
#   powershell -NoProfile -File tools\check_contract_parity.ps1 -RinhaRoot D:\path\to\RinhaReplacement
#
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$RinhaRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
        $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
    }
}

if ([string]::IsNullOrWhiteSpace($RinhaRoot)) {
    # staging_releases/GakuMotion -> staging_releases -> repo root -> inventory
    $parent = Split-Path -Parent $RepoRoot
    $grandParent = Split-Path -Parent $parent
    $RinhaRoot = Join-Path $grandParent 'inventory\RinhaReplacement'
}

Write-Host "GakuMotion : $RepoRoot"
Write-Host "Rinha      : $RinhaRoot"

if (-not (Test-Path -LiteralPath $RinhaRoot)) {
    Write-Host "  SKIP: RinhaReplacement checkout not found; nothing to compare against." -ForegroundColor Yellow
    exit 0
}

$pairs = @(
    @{ Name = 'CharaAdapter service contract'; Mine = 'include\GakuCharaAdapter.h';
       Theirs = 'include\GakuCharaAdapter.h' },
    @{ Name = 'service contract'; Mine = 'include\RinhaFacialFit.h';
       Theirs = 'include\RinhaFacialFit.h' },
    @{ Name = 'eye key projection'; Mine = 'tools\vmd2gkmotion\miku_raw_eye_keys.json';
       Theirs = 'data\facial_fit\runtime\krnh_v1.eye_keys.json' }
)

$failures = 0
foreach ($pair in $pairs) {
    $mine = Join-Path $RepoRoot $pair.Mine
    $theirs = Join-Path $RinhaRoot $pair.Theirs
    if (-not (Test-Path -LiteralPath $mine)) { Write-Host "  MISSING (GakuMotion): $($pair.Mine)" -ForegroundColor Red; $failures++; continue }
    if (-not (Test-Path -LiteralPath $theirs)) { Write-Host "  MISSING (Rinha): $($pair.Theirs)" -ForegroundColor Red; $failures++; continue }
    $left = (Get-FileHash -LiteralPath $mine -Algorithm SHA256).Hash
    $right = (Get-FileHash -LiteralPath $theirs -Algorithm SHA256).Hash
    if ($left -ne $right) {
        Write-Host "  DRIFT: $($pair.Name)" -ForegroundColor Red
        Write-Host "    GakuMotion $($pair.Mine)  $left"
        Write-Host "    Rinha      $($pair.Theirs)  $right"
        $failures++
        continue
    }
    Write-Host ("  OK: {0} ({1})" -f $pair.Name, $left.Substring(0, 16)) -ForegroundColor Green
}

if ($failures -ne 0) {
    Write-Host "Facial-fit contract parity FAILED ($failures)." -ForegroundColor Red
    exit 1
}

Write-Host 'Facial-fit contract parity passed.'
exit 0
