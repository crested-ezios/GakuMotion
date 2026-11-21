#Requires -Version 5.1
# NOTE: Keep this script strictly ASCII-only (Windows PowerShell 5.1 ANSI parsing).

[CmdletBinding()]
param(
    [ValidateSet('1', '2', '3', '4', 'All')]
    [string]$Tier = 'All',
    [string]$OutJson = '',
    [string]$BuildDir = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonPathsScript = Join-Path (Split-Path -Parent $PSScriptRoot) 'common\e2e_paths.ps1'
if (-not (Test-Path -LiteralPath $commonPathsScript -PathType Leaf)) {
    throw "Shared E2E helper not found: '$commonPathsScript'"
}
. $commonPathsScript

$paths = Get-E2EPaths
$resolvedBuildDir = if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    Join-Path $paths.BuildRoot 'multi_actor'
} else {
    [System.IO.Path]::GetFullPath($BuildDir)
}
if (-not (Test-Path -LiteralPath $resolvedBuildDir -PathType Container)) {
    New-Item -ItemType Directory -Path $resolvedBuildDir -Force | Out-Null
}

$resolvedOutJson = if ([string]::IsNullOrWhiteSpace($OutJson)) {
    Join-Path $resolvedBuildDir 'suite_results.json'
} else {
    [System.IO.Path]::GetFullPath($OutJson)
}
$outParent = [System.IO.Path]::GetDirectoryName($resolvedOutJson)
if (-not [string]::IsNullOrWhiteSpace($outParent) -and -not (Test-Path -LiteralPath $outParent -PathType Container)) {
    New-Item -ItemType Directory -Path $outParent -Force | Out-Null
}
if (Test-Path -LiteralPath $resolvedOutJson -PathType Leaf) {
    Remove-Item -LiteralPath $resolvedOutJson -Force
}

$toolchain = Resolve-E2EToolchain -RepoRoot $paths.RepoRoot

function Invoke-MultiActorBinary {
    param(
        [Parameter(Mandatory = $true)][string]$SourceFile,
        [Parameter(Mandatory = $true)][string]$SubBuildDir,
        [Parameter(Mandatory = $true)][string]$ExeName,
        [Parameter(Mandatory = $true)][string]$JsonName,
        [switch]$UseImGui
    )

    if (-not (Test-Path -LiteralPath $SubBuildDir -PathType Container)) {
        New-Item -ItemType Directory -Path $SubBuildDir -Force | Out-Null
    }
    $exePath = Join-Path $SubBuildDir $ExeName
    $jsonPath = Join-Path $SubBuildDir $JsonName
    if (Test-Path -LiteralPath $jsonPath -PathType Leaf) {
        Remove-Item -LiteralPath $jsonPath -Force
    }

    $res = Invoke-E2ECppBuildAndRun `
        -SourceFiles @($SourceFile) `
        -ExePath $exePath `
        -BuildDir $SubBuildDir `
        -UseSrcDefaults `
        -ImGui:$UseImGui `
        -WarningLevel '/W3' `
        -RunArgs @('--out', $jsonPath, '--tier', $Tier, '--allow-pending') `
        -Toolchain $toolchain

    if (-not [string]::IsNullOrWhiteSpace($res.RunOutput)) {
        Write-Host $res.RunOutput
    }

    if ($res.RunExitCode -eq 2 -or $res.RunExitCode -lt 0 -or $res.RunExitCode -gt 2) {
        throw "Multi-actor binary '$ExeName' exited with infrastructure/runtime code $($res.RunExitCode)."
    }
    if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        throw "Multi-actor binary '$ExeName' did not produce output JSON '$jsonPath' (exit=$($res.RunExitCode))."
    }

    $rawJson = Read-Utf8Text -Path $jsonPath
    $records = ConvertFrom-E2EJsonArray -JsonText $rawJson

    if ($res.RunExitCode -eq 1 -and $records.Count -eq 0) {
        Write-Host "[multi_actor] '$ExeName' matched 0 tests for Tier=$Tier (allowed empty slice)."
        return @()
    }

    if ($res.RunExitCode -ne 0) {
        $hasFailures = @($records | Where-Object { $_.status -eq 'FAIL' }).Count -gt 0
        if (-not $hasFailures) {
            throw "Multi-actor binary '$ExeName' exited with code $($res.RunExitCode) without FAIL records."
        }
    }

    return ,$records
}

$coreSource = Join-Path $PSScriptRoot 'test_multi_actor_core.cpp'
$imguiSource = Join-Path $PSScriptRoot 'test_multi_actor_imgui.cpp'

$hadEnvTmp = Test-Path -LiteralPath 'Env:GKE2E_TMP_DIR'
$prevEnvTmp = if ($hadEnvTmp) { $env:GKE2E_TMP_DIR } else { $null }
$isolatedTmpDir = Join-Path $resolvedBuildDir 'tmp'
$env:GKE2E_TMP_DIR = $isolatedTmpDir

try {
    $coreRecords = Invoke-MultiActorBinary `
        -SourceFile $coreSource `
        -SubBuildDir (Join-Path $resolvedBuildDir 'core_build') `
        -ExeName 'test_multi_actor_core.exe' `
        -JsonName 'core_results.json'

    $imguiRecords = Invoke-MultiActorBinary `
        -SourceFile $imguiSource `
        -SubBuildDir (Join-Path $resolvedBuildDir 'imgui_build') `
        -ExeName 'test_multi_actor_imgui.exe' `
        -JsonName 'imgui_results.json' `
        -UseImGui
} finally {
    if ($hadEnvTmp) {
        $env:GKE2E_TMP_DIR = $prevEnvTmp
    } else {
        Remove-Item -LiteralPath 'Env:GKE2E_TMP_DIR' -ErrorAction SilentlyContinue
    }
    if ((Test-Path -LiteralPath $isolatedTmpDir -PathType Container) -and
        @(Get-ChildItem -LiteralPath $isolatedTmpDir -Force -ErrorAction SilentlyContinue).Count -eq 0) {
        Remove-Item -LiteralPath $isolatedTmpDir -Force -ErrorAction SilentlyContinue
    }
}

$merged = New-Object System.Collections.Generic.List[object]
foreach ($rec in $coreRecords) {
    $merged.Add([pscustomobject]@{
        id      = [string]$rec.id
        tier    = [int]$rec.tier
        feature = [string]$rec.feature
        name    = [string]$rec.name
        status  = [string]$rec.status
        detail  = [string]$rec.detail
    })
}
foreach ($rec in $imguiRecords) {
    $merged.Add([pscustomobject]@{
        id      = [string]$rec.id
        tier    = [int]$rec.tier
        feature = [string]$rec.feature
        name    = [string]$rec.name
        status  = [string]$rec.status
        detail  = [string]$rec.detail
    })
}

if ($merged.Count -eq 0) {
    throw "Multi-actor suite produced 0 test records for Tier=$Tier."
}

$jsonText = ConvertTo-Json -InputObject @($merged.ToArray()) -Depth 5
$trimmed = $jsonText.TrimStart()
if (-not $trimmed.StartsWith('[')) {
    $jsonText = "[$jsonText]"
}

Write-Utf8NoBomText -Path $resolvedOutJson -Content ($jsonText + "`n")
Write-Host "[multi_actor] Merged $($merged.Count) test records -> '$resolvedOutJson'."
exit 0
