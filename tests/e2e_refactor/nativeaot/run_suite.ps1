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

. (Join-Path $PSScriptRoot '..\common\e2e_paths.ps1')

$paths = Get-E2EPaths
$toolchain = Resolve-E2EToolchain -RepoRoot $paths.RepoRoot

$resolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$pyWorkDir = Join-Path $resolvedBuildDir 'py_work'
$cppWorkDir = Join-Path $resolvedBuildDir 'cpp_work'
$cppBuildDir = Join-Path $resolvedBuildDir 'cpp_build'

foreach ($d in @($resolvedBuildDir, $pyWorkDir, $cppWorkDir, $cppBuildDir)) {
    if (-not (Test-Path -LiteralPath $d -PathType Container)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

$pyJsonPath = Join-Path $resolvedBuildDir 'py_results.json'
$cppJsonPath = Join-Path $resolvedBuildDir 'cpp_results.json'
foreach ($stale in @($pyJsonPath, $cppJsonPath)) {
    if (Test-Path -LiteralPath $stale -PathType Leaf) {
        Remove-Item -LiteralPath $stale -Force
    }
}

# 1. Run Python ctypes NativeAOT suite (F2, F3, T3-F2F3, T4-NA-PY)
if (-not $toolchain.PythonExe) {
    throw 'Python >= 3.8 executable not found for nativeaot E2E suite.'
}

$pyScript = Join-Path $PSScriptRoot 'test_nativeaot_import.py'
if (-not (Test-Path -LiteralPath $pyScript -PathType Leaf)) {
    throw "Python test script not found: '$pyScript'"
}

$pyArgs = @($toolchain.PythonArgsPrefix) + @(
    $pyScript,
    '--dll', $paths.NativeAotDll,
    '--tier', $Tier,
    '--out', $pyJsonPath,
    '--work-dir', $pyWorkDir
)

Write-Host "[nativeaot] Running Python ctypes suite (Tier=$Tier)..."
$pyExit = 0
$pyOutputText = ''
$oldPref = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $global:LASTEXITCODE = 0
    $pyOut = & $toolchain.PythonExe @pyArgs 2>&1
    $pyExit = [int]$LASTEXITCODE
    $pyOutputText = ($pyOut | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
} finally {
    $ErrorActionPreference = $oldPref
}
if (-not [string]::IsNullOrWhiteSpace($pyOutputText)) {
    Write-Host $pyOutputText
}
if ($pyExit -notin @(0, 1)) {
    throw "Python NativeAOT suite failed with infrastructure exit code $pyExit.`n$pyOutputText"
}
if (-not (Test-Path -LiteralPath $pyJsonPath -PathType Leaf)) {
    throw "Python NativeAOT suite did not produce output JSON '$pyJsonPath' (exit=$pyExit)."
}

# 2. Build & run C++20 src/ library suite (F4, T3-F2F4, T3-F3F4, T4-NA-CPP)
$cppSource = Join-Path $PSScriptRoot 'test_group_library.cpp'
if (-not (Test-Path -LiteralPath $cppSource -PathType Leaf)) {
    throw "C++ test source not found: '$cppSource'"
}
$cppExe = Join-Path $cppBuildDir 'e2e_nativeaot_cpp.exe'

Write-Host "[nativeaot] Compiling and running C++20 library suite (Tier=$Tier)..."
$cppResult = Invoke-E2ECppBuildAndRun `
    -SourceFiles @($cppSource) `
    -ExePath $cppExe `
    -BuildDir $cppBuildDir `
    -IncludeDirs @($PSScriptRoot) `
    -UseSrcDefaults `
    -WarningLevel '/W3' `
    -RunArgs @(
        '--out', $cppJsonPath,
        '--tier', $Tier,
        '--allow-pending',
        '--nativeaot-dir', $paths.NativeAotDir,
        '--work-dir', $cppWorkDir
    ) `
    -Toolchain $toolchain

if (-not [string]::IsNullOrWhiteSpace($cppResult.RunOutput)) {
    Write-Host $cppResult.RunOutput
}
if ($cppResult.RunExitCode -notin @(0, 1)) {
    throw "C++ NativeAOT/Library suite exited with code $($cppResult.RunExitCode)."
}
if (-not (Test-Path -LiteralPath $cppJsonPath -PathType Leaf)) {
    throw "C++ NativeAOT/Library suite did not produce output JSON '$cppJsonPath'."
}

# 3. Validate and merge both JSON arrays cleanly
$pyRecords = ConvertFrom-E2EJsonArray -JsonText (Read-Utf8Text -Path $pyJsonPath)
$cppRecords = ConvertFrom-E2EJsonArray -JsonText (Read-Utf8Text -Path $cppJsonPath)

$merged = New-Object System.Collections.Generic.List[object]
$seenIds = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
$requiredProps = @('id', 'tier', 'feature', 'name', 'status', 'detail')
$allowedStatuses = @('PASS', 'FAIL', 'PENDING_IMPL')

foreach ($rec in (@($pyRecords) + @($cppRecords))) {
    if ($null -eq $rec) {
        throw 'Merged test results contain a null record.'
    }
    foreach ($rp in $requiredProps) {
        if ($null -eq $rec.PSObject.Properties[$rp]) {
            throw "Test record missing required property '$rp'."
        }
    }
    $idStr = [string]$rec.id
    if ([string]::IsNullOrWhiteSpace($idStr)) {
        throw 'Test record has empty id.'
    }
    if (-not $seenIds.Add($idStr)) {
        throw "Duplicate test record id detected: '$idStr'."
    }
    $statusStr = [string]$rec.status
    if ($allowedStatuses -notcontains $statusStr) {
        throw "Test record '$idStr' has invalid status '$statusStr'."
    }
    $tierVal = [int]$rec.tier
    if ($tierVal -lt 1 -or $tierVal -gt 4) {
        throw "Test record '$idStr' has invalid tier '$($rec.tier)'."
    }

    $merged.Add([pscustomobject]@{
        id      = $idStr
        tier    = $tierVal
        feature = [string]$rec.feature
        name    = [string]$rec.name
        status  = $statusStr
        detail  = [string]$rec.detail
    })
}

if ($merged.Count -eq 0) {
    throw "Merged test result array is empty for Tier=$Tier."
}

$mergedJson = ConvertTo-Json -InputObject @($merged.ToArray()) -Depth 5
Write-Utf8NoBomText -Path $OutJson -Content $mergedJson
exit 0
