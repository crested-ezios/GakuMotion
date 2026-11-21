#Requires -Version 5.1
# NOTE: Keep this script strictly ASCII-only (Windows PowerShell 5.1 ANSI parsing).

[CmdletBinding()]
param(
    [ValidateSet('1', '2', '3', '4', 'All')]
    [string]$Tier = 'All',
    [Parameter(Mandatory = $true)][string]$OutJson,
    [Parameter(Mandatory = $true)][string]$BuildDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonScript = Join-Path (Split-Path -Parent $PSScriptRoot) 'common\e2e_paths.ps1'
. $commonScript

$paths = Get-E2EPaths
$resolvedOutJson = [System.IO.Path]::GetFullPath($OutJson)
$resolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)

if (-not (Test-Path -LiteralPath $resolvedBuildDir -PathType Container)) {
    New-Item -ItemType Directory -Path $resolvedBuildDir -Force | Out-Null
}
$outParent = [System.IO.Path]::GetDirectoryName($resolvedOutJson)
if (-not [string]::IsNullOrWhiteSpace($outParent) -and -not (Test-Path -LiteralPath $outParent -PathType Container)) {
    New-Item -ItemType Directory -Path $outParent -Force | Out-Null
}
if (Test-Path -LiteralPath $resolvedOutJson -PathType Leaf) {
    Remove-Item -LiteralPath $resolvedOutJson -Force
}

$tc = $null
try {
    $tc = Resolve-E2EToolchain -RepoRoot $paths.RepoRoot
} catch {
    $tc = $null
}

$nodeCmd = if ($tc -and $tc.NodeExe) { $tc.NodeExe } else {
    $cmd = Get-Command 'node' -ErrorAction SilentlyContinue
    if ($cmd) { $cmd.Source } else { $null }
}

if (-not $nodeCmd -or -not $paths.HasFrontendDeps) {
    $detail = if (-not $nodeCmd) {
        'Node.js executable not found in PATH.'
    } else {
        "Frontend dependencies (vitest/vite) not installed under '$($paths.FrontendDir)'."
    }
    $errRecord = @(
        [pscustomobject]@{
            id      = 'ENV-FRONTEND-DEPS'
            tier    = 1
            feature = 'F14'
            name    = 'Frontend Node/Vitest dependencies check'
            status  = 'FAIL'
            detail  = $detail
        }
    )
    $errJson = ConvertTo-Json -InputObject $errRecord -Depth 4
    Write-Utf8NoBomText -Path $resolvedOutJson -Content ($errJson + "`n")
    exit 1
}

# For Tier 3, 4, or All, compile and run C++ GkWebPanelBridge probe if MSVC is available.
$bridgeJsonPath = Join-Path $resolvedBuildDir 'bridge_wire_fixtures.json'
if (Test-Path -LiteralPath $bridgeJsonPath -PathType Leaf) {
    Remove-Item -LiteralPath $bridgeJsonPath -Force
}

if ($Tier -in @('3', '4', 'All')) {
    if ($tc -and $tc.ClExe) {
        $probeCpp = Join-Path $PSScriptRoot 'frontend_bridge_probe.cpp'
        $probeExe = Join-Path $resolvedBuildDir 'frontend_bridge_probe.exe'
        try {
            $probeRes = Invoke-E2ECppBuildAndRun `
                -SourceFiles @($probeCpp) `
                -ExePath $probeExe `
                -BuildDir $resolvedBuildDir `
                -WarningLevel '/W3' `
                -UseSrcDefaults `
                -RunArgs @('--out', $bridgeJsonPath) `
                -Toolchain $tc
            if ($probeRes.RunExitCode -ne 0 -or -not (Test-Path -LiteralPath $bridgeJsonPath -PathType Leaf)) {
                $fallback = [pscustomobject]@{
                    m4Ready = $false
                    reason  = "frontend_bridge_probe.exe exited $($probeRes.RunExitCode)"
                } | ConvertTo-Json -Depth 4
                Write-Utf8NoBomText -Path $bridgeJsonPath -Content ($fallback + "`n")
            }
        } catch {
            $errMsg = $_.Exception.Message -replace "`r|`n", ' '
            $fallback = [pscustomobject]@{
                m4Ready = $false
                reason  = "C++ bridge probe unavailable: $errMsg"
            } | ConvertTo-Json -Depth 4
            Write-Utf8NoBomText -Path $bridgeJsonPath -Content ($fallback + "`n")
        }
    } else {
        $fallback = [pscustomobject]@{
            m4Ready = $false
            reason  = 'MSVC cl.exe not available for C++ bridge probe'
        } | ConvertTo-Json -Depth 4
        Write-Utf8NoBomText -Path $bridgeJsonPath -Content ($fallback + "`n")
    }
}

$vitestMjs = Join-Path $paths.FrontendDir 'node_modules\vitest\vitest.mjs'
$vitestCfg = Join-Path $PSScriptRoot 'vitest.config.mjs'

$envBackup = @{
    GKM_E2E_OUT_JSON    = $env:GKM_E2E_OUT_JSON
    GKM_E2E_TIER        = $env:GKM_E2E_TIER
    GKM_E2E_BUILD_DIR   = $env:GKM_E2E_BUILD_DIR
    GKM_E2E_BRIDGE_JSON = $env:GKM_E2E_BRIDGE_JSON
}

$vitestExit = 0
try {
    $env:GKM_E2E_OUT_JSON = $resolvedOutJson
    $env:GKM_E2E_TIER = $Tier
    $env:GKM_E2E_BUILD_DIR = $resolvedBuildDir
    $env:GKM_E2E_BRIDGE_JSON = $bridgeJsonPath

    $oldPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $vitestOut = & $nodeCmd $vitestMjs run --config $vitestCfg --no-color 2>&1
        $vitestExit = [int]$LASTEXITCODE
        foreach ($line in $vitestOut) {
            Write-Host ($line.ToString())
        }
    } finally {
        $ErrorActionPreference = $oldPref
    }
} finally {
    foreach ($key in $envBackup.Keys) {
        $val = $envBackup[$key]
        if ($null -eq $val) {
            Remove-Item -LiteralPath ("Env:{0}" -f $key) -ErrorAction SilentlyContinue
        } else {
            [System.Environment]::SetEnvironmentVariable($key, $val, 'Process')
        }
    }
}

if (-not (Test-Path -LiteralPath $resolvedOutJson -PathType Leaf)) {
    Write-Error "Vitest did not produce output JSON '$resolvedOutJson' (vitest exit=$vitestExit)."
    exit 1
}

try {
    $rawJson = Read-Utf8Text -Path $resolvedOutJson
    $records = ConvertFrom-E2EJsonArray -JsonText $rawJson
    if ($records.Count -eq 0) {
        throw 'Output JSON array is empty.'
    }
    $allowedStatus = @('PASS', 'FAIL', 'PENDING_IMPL')
    $allowedFeatures = @('F13', 'F14', 'F15', 'F16')
    foreach ($rec in $records) {
        foreach ($prop in @('id', 'tier', 'feature', 'name', 'status', 'detail')) {
            if ($null -eq $rec.PSObject.Properties[$prop]) {
                throw "Record missing property '$prop'."
            }
        }
        if ($allowedStatus -notcontains [string]$rec.status) {
            throw "Invalid status '$($rec.status)' on record '$($rec.id)'."
        }
        if ($allowedFeatures -notcontains [string]$rec.feature) {
            throw "Invalid feature '$($rec.feature)' on record '$($rec.id)'."
        }
        $tNum = [int]$rec.tier
        if ($tNum -lt 1 -or $tNum -gt 4) {
            throw "Invalid tier '$($rec.tier)' on record '$($rec.id)'."
        }
    }
} catch {
    Write-Error "Invalid output JSON '$resolvedOutJson': $($_.Exception.Message)"
    exit 1
}

# Valid structured JSON produced; exit 0 so run_e2e_suite.ps1 handles PASS/FAIL/PENDING_IMPL gating.
exit 0
