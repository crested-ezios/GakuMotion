#Requires -Version 5.1
# NOTE: Keep this script strictly ASCII-only (Windows PowerShell 5.1 ANSI parsing).

[CmdletBinding()]
param(
    [ValidateSet('1', '2', '3', '4', 'All')]
    [string]$Tier = 'All',
    [switch]$AllowPending,
    [string[]]$Domain = @(),
    [string]$BuildRoot = '',
    [string]$SuitesRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common\e2e_paths.ps1')

$paths = Get-E2EPaths -BuildRoot $BuildRoot
$resolvedBuildRoot = $paths.BuildRoot
$resolvedSuitesRoot = if ([string]::IsNullOrWhiteSpace($SuitesRoot)) {
    $PSScriptRoot
} else {
    [System.IO.Path]::GetFullPath($SuitesRoot)
}

if (-not (Test-Path -LiteralPath $resolvedSuitesRoot -PathType Container)) {
    throw "SuitesRoot directory does not exist: '$resolvedSuitesRoot'"
}

# Normalize -Domain filter (supports array elements or comma-separated tokens)
$domainFilters = New-Object System.Collections.Generic.List[string]
foreach ($d in $Domain) {
    foreach ($part in ($d -split ',')) {
        $trimmed = $part.Trim()
        if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
            $domainFilters.Add($trimmed)
        }
    }
}

# Discover immediate subdirectories excluding 'common' and '.' / '_' prefixed dirs
$discoveredDirs = @(Get-ChildItem -LiteralPath $resolvedSuitesRoot -Directory |
    Where-Object {
        $_.Name -ne 'common' -and
        -not $_.Name.StartsWith('.') -and
        -not $_.Name.StartsWith('_') -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'run_suite.ps1') -PathType Leaf)
    } |
    Sort-Object -Property Name)

$selectedDirs = @()
if ($domainFilters.Count -gt 0) {
    $selectedDirs = @($discoveredDirs | Where-Object {
        $name = $_.Name
        @($domainFilters | Where-Object { $_ -ieq $name }).Count -gt 0
    })
} else {
    $selectedDirs = $discoveredDirs
}

$allResults = New-Object System.Collections.Generic.List[object]
$suiteSummaries = New-Object System.Collections.Generic.List[object]
$suiteErrors = 0

# Flag any requested -Domain entries that do not correspond to a discovered suite
foreach ($reqDomain in $domainFilters) {
    $matched = @($discoveredDirs | Where-Object { $_.Name -ieq $reqDomain })
    if ($matched.Count -eq 0) {
        $suiteErrors++
        $errMsg = "Requested domain suite '$reqDomain' not found under '$resolvedSuitesRoot'."
        $allResults.Add([pscustomobject]@{
            id      = "SUITE-$reqDomain-MISSING"
            tier    = 1
            feature = 'SUITE'
            name    = "Domain suite discovery ($reqDomain)"
            status  = 'FAIL'
            detail  = $errMsg
        })
        $suiteSummaries.Add([pscustomobject]@{
            domain   = $reqDomain
            exitCode = 1
            ok       = $false
            count    = 1
            detail   = $errMsg
        })
    }
}

function Add-SyntheticSuiteFailure {
    param(
        [string]$DomainName,
        [string]$IdSuffix,
        [string]$Detail
    )
    $allResults.Add([pscustomobject]@{
        id      = "SUITE-$DomainName-$IdSuffix"
        tier    = 1
        feature = 'SUITE'
        name    = "Domain suite execution ($DomainName)"
        status  = 'FAIL'
        detail  = $Detail
    })
}

$psExe = (Get-Command 'powershell.exe' -ErrorAction SilentlyContinue)
$psExePath = if ($psExe) { $psExe.Source } else { 'powershell.exe' }

foreach ($suiteDir in $selectedDirs) {
    $domainName = $suiteDir.Name
    $suiteScript = Join-Path $suiteDir.FullName 'run_suite.ps1'
    $domainBuildDir = Join-Path $resolvedBuildRoot $domainName
    if (-not (Test-Path -LiteralPath $domainBuildDir -PathType Container)) {
        New-Item -ItemType Directory -Path $domainBuildDir -Force | Out-Null
    }
    $outJsonPath = Join-Path $domainBuildDir 'suite_results.json'
    if (Test-Path -LiteralPath $outJsonPath -PathType Leaf) {
        Remove-Item -LiteralPath $outJsonPath -Force
    }

    Write-Host "[E2E] Running domain suite '$domainName' (Tier=$Tier)..."
    $suiteExit = 0
    $suiteOutputText = ''
    $oldPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $childOut = & $psExePath -NoProfile -ExecutionPolicy Bypass -File $suiteScript -Tier $Tier -OutJson $outJsonPath -BuildDir $domainBuildDir 2>&1
        $suiteExit = [int]$LASTEXITCODE
        $suiteOutputText = ($childOut | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    } finally {
        $ErrorActionPreference = $oldPref
    }
    if (-not [string]::IsNullOrWhiteSpace($suiteOutputText)) {
        Write-Host $suiteOutputText
    }

    $parsedValid = $false
    $domainRecords = @()
    $jsonError = ''

    if (-not (Test-Path -LiteralPath $outJsonPath -PathType Leaf)) {
        $jsonError = "Missing output JSON '$outJsonPath' (exit=$suiteExit)."
    } else {
        try {
            $rawJson = Read-Utf8Text -Path $outJsonPath
            $domainRecords = ConvertFrom-E2EJsonArray -JsonText $rawJson
            $requiredProps = @('id', 'tier', 'feature', 'name', 'status', 'detail')
            $allowedStatuses = @('PASS', 'FAIL', 'PENDING_IMPL')
            foreach ($rec in $domainRecords) {
                if ($null -eq $rec) {
                    throw 'Output JSON contains a null element.'
                }
                foreach ($rp in $requiredProps) {
                    if ($null -eq $rec.PSObject.Properties[$rp]) {
                        throw "Test record missing required property '$rp'."
                    }
                }
                $st = [string]$rec.status
                if ($allowedStatuses -notcontains $st) {
                    throw "Test record '$($rec.id)' has invalid status '$st'."
                }
                $tierNum = [int]$rec.tier
                if ($tierNum -lt 1 -or $tierNum -gt 4) {
                    throw "Test record '$($rec.id)' has invalid tier '$($rec.tier)'."
                }
            }
            $parsedValid = $true
        } catch {
            $jsonError = "Malformed output JSON '$outJsonPath': $($_.Exception.Message)"
        }
    }

    if ($parsedValid) {
        foreach ($rec in $domainRecords) {
            $allResults.Add([pscustomobject]@{
                id      = [string]$rec.id
                tier    = [int]$rec.tier
                feature = [string]$rec.feature
                name    = [string]$rec.name
                status  = [string]$rec.status
                detail  = [string]$rec.detail
            })
        }
    } else {
        $suiteErrors++
        Add-SyntheticSuiteFailure -DomainName $domainName -IdSuffix 'JSON' -Detail $jsonError
    }

    if ($suiteExit -ne 0) {
        $suiteErrors++
        Add-SyntheticSuiteFailure -DomainName $domainName -IdSuffix 'EXIT' -Detail "Domain suite '$domainName' exited with non-zero code $suiteExit."
    }

    $suiteSummaries.Add([pscustomobject]@{
        domain   = $domainName
        exitCode = $suiteExit
        ok       = ($suiteExit -eq 0 -and $parsedValid)
        count    = $domainRecords.Count
        outJson  = $outJsonPath
        detail   = if ($parsedValid -and $suiteExit -eq 0) { 'OK' } elseif (-not $parsedValid) { $jsonError } else { "Non-zero exit $suiteExit" }
    })
}

$resultsArr = @($allResults.ToArray())

# Aggregate Tier 1-4 stats
$tierStats = @()
foreach ($t in @(1, 2, 3, 4)) {
    $tItems = @($resultsArr | Where-Object { [int]$_.tier -eq $t })
    $tPass = @($tItems | Where-Object { $_.status -eq 'PASS' }).Count
    $tFail = @($tItems | Where-Object { $_.status -eq 'FAIL' }).Count
    $tPend = @($tItems | Where-Object { $_.status -eq 'PENDING_IMPL' }).Count
    $tierStats += [pscustomobject]@{
        tier         = $t
        label        = "Tier $t"
        pass         = $tPass
        fail         = $tFail
        pending_impl = $tPend
        total        = $tItems.Count
    }
}

$passCount = @($resultsArr | Where-Object { $_.status -eq 'PASS' }).Count
$failCount = @($resultsArr | Where-Object { $_.status -eq 'FAIL' }).Count
$pendingCount = @($resultsArr | Where-Object { $_.status -eq 'PENDING_IMPL' }).Count
$totalCount = $resultsArr.Count

# Aggregate Feature F2..F16 stats
$featureList = @('F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12', 'F13', 'F14', 'F15', 'F16')
$featureStats = @()
foreach ($feat in $featureList) {
    $fItems = @($resultsArr | Where-Object { $_.feature -eq $feat })
    $ft1 = @($fItems | Where-Object { [int]$_.tier -eq 1 }).Count
    $ft2 = @($fItems | Where-Object { [int]$_.tier -eq 2 }).Count
    $ft3 = @($fItems | Where-Object { [int]$_.tier -eq 3 }).Count
    $ft4 = @($fItems | Where-Object { [int]$_.tier -eq 4 }).Count
    $fPass = @($fItems | Where-Object { $_.status -eq 'PASS' }).Count
    $fFail = @($fItems | Where-Object { $_.status -eq 'FAIL' }).Count
    $fPend = @($fItems | Where-Object { $_.status -eq 'PENDING_IMPL' }).Count
    $featureStats += [pscustomobject]@{
        feature      = $feat
        tier1        = $ft1
        tier2        = $ft2
        tier3        = $ft3
        tier4        = $ft4
        pass         = $fPass
        fail         = $fFail
        pending_impl = $fPend
        total        = $fItems.Count
    }
}

# Print ASCII-clean Tier 1-4 Summary Table
Write-Host ''
Write-Host '=== E2E Tier Summary ==='
Write-Host ('{0,-8} | {1,6} | {2,6} | {3,12} | {4,6}' -f 'Tier', 'PASS', 'FAIL', 'PENDING_IMPL', 'Total')
Write-Host ('-' * 48)
foreach ($row in $tierStats) {
    Write-Host ('{0,-8} | {1,6} | {2,6} | {3,12} | {4,6}' -f $row.label, $row.pass, $row.fail, $row.pending_impl, $row.total)
}
Write-Host ('-' * 48)
Write-Host ('{0,-8} | {1,6} | {2,6} | {3,12} | {4,6}' -f 'Total', $passCount, $failCount, $pendingCount, $totalCount)

# Print ASCII-clean Feature F2..F16 Summary Table
Write-Host ''
Write-Host '=== E2E Feature Summary (F2..F16) ==='
Write-Host ('{0,-7} | {1,4} | {2,4} | {3,4} | {4,4} | {5,6} | {6,6} | {7,12} | {8,6}' -f 'Feature', 'T1', 'T2', 'T3', 'T4', 'PASS', 'FAIL', 'PENDING_IMPL', 'Total')
Write-Host ('-' * 74)
foreach ($row in $featureStats) {
    Write-Host ('{0,-7} | {1,4} | {2,4} | {3,4} | {4,4} | {5,6} | {6,6} | {7,12} | {8,6}' -f $row.feature, $row.tier1, $row.tier2, $row.tier3, $row.tier4, $row.pass, $row.fail, $row.pending_impl, $row.total)
}

# Exit code contract
if ($AllowPending) {
    $finalExitCode = if ($suiteErrors -eq 0 -and $failCount -eq 0 -and ($passCount + $pendingCount) -gt 0) { 0 } else { 1 }
} else {
    $finalExitCode = if ($suiteErrors -eq 0 -and $failCount -eq 0 -and $pendingCount -eq 0 -and $passCount -gt 0) { 0 } else { 1 }
}

$summaryObj = [pscustomobject]@{
    timestamp_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    mode          = if ($AllowPending) { 'allow_pending' } else { 'strict' }
    tier_filter   = $Tier
    suites_root   = $resolvedSuitesRoot
    build_root    = $resolvedBuildRoot
    suite_errors  = $suiteErrors
    suites        = @($suiteSummaries.ToArray())
    totals        = [pscustomobject]@{
        pass         = $passCount
        fail         = $failCount
        pending_impl = $pendingCount
        total        = $totalCount
    }
    tiers         = $tierStats
    features      = $featureStats
    results       = $resultsArr
    exit_code     = $finalExitCode
}

$summaryJsonPath = Join-Path $resolvedBuildRoot 'e2e_summary.json'
$summaryJsonText = $summaryObj | ConvertTo-Json -Depth 10
Write-Utf8NoBomText -Path $summaryJsonPath -Content $summaryJsonText
Write-Host ''
Write-Host ("[E2E] Summary written to '{0}' (mode={1}, PASS={2}, FAIL={3}, PENDING_IMPL={4}, suiteErrors={5}, exit={6})" -f `
    $summaryJsonPath, $summaryObj.mode, $passCount, $failCount, $pendingCount, $suiteErrors, $finalExitCode)

exit $finalExitCode
