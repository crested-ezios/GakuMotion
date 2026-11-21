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

. (Join-Path $PSScriptRoot '..\common\e2e_paths.ps1')

$paths = Get-E2EPaths
$toolchain = Resolve-E2EToolchain -RepoRoot $paths.RepoRoot

$resolvedBuildDir = if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    Join-Path $paths.BuildRoot 'portrait_bridge'
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

$octoExtractedDir = Join-Path $resolvedBuildDir 'octo_extracted'

# Step 1: Read-only offline Octo extraction (strictly for Tier 4 / All when cache exists)
if ($paths.HasGameOctoCache -and ($Tier -eq '4' -or $Tier -eq 'All')) {
    if (-not $toolchain.PythonExe) {
        throw 'Python executable not found for octo_extract_helper.py.'
    }
    $helperPy = Join-Path $PSScriptRoot 'octo_extract_helper.py'
    $pyArgs = @($toolchain.PythonArgsPrefix) + @(
        $helperPy,
        '--game-root', $paths.GameRoot,
        '--out-dir', $octoExtractedDir
    )
    $oldPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $pyOut = & $toolchain.PythonExe @pyArgs 2>&1
        $pyExit = [int]$LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPref
    }
    $pyText = ($pyOut | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    if (-not [string]::IsNullOrWhiteSpace($pyText)) {
        Write-Host $pyText
    }
    if ($pyExit -ne 0) {
        throw "octo_extract_helper.py failed (exit=$pyExit):`n$pyText"
    }
}

# Step 2: Compile & run C++20 /W3 harness
$cppSrc = Join-Path $PSScriptRoot 'portrait_bridge_tests.cpp'
$cppExe = Join-Path $resolvedBuildDir 'portrait_bridge_tests.exe'
$cppJson = Join-Path $resolvedBuildDir 'cpp_results.json'
if (Test-Path -LiteralPath $cppJson -PathType Leaf) {
    Remove-Item -LiteralPath $cppJson -Force
}

$prevBuildDirEnv = $env:GKE2E_BUILD_DIR
$prevOctoDirEnv = $env:GKE2E_OCTO_EXTRACTED_DIR
$prevGameRootEnv = $env:GAKUMOTION_GAME_ROOT
$prevTsReportEnv = $env:GKE2E_TS_REPORT_JSON
$prevTierFilterEnv = $env:GKE2E_TIER_FILTER
$suiteInfraExit = 0

try {
    $env:GKE2E_BUILD_DIR = $resolvedBuildDir
    $env:GKE2E_OCTO_EXTRACTED_DIR = $octoExtractedDir
    $env:GAKUMOTION_GAME_ROOT = $paths.GameRoot

    $cppResult = Invoke-E2ECppBuildAndRun `
        -SourceFiles @($cppSrc) `
        -ExePath $cppExe `
        -BuildDir $resolvedBuildDir `
        -UseSrcDefaults `
        -WarningLevel '/W3' `
        -Libs @('winmm.lib', 'user32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'comdlg32.lib', 'windowscodecs.lib', 'bcrypt.lib', 'advapi32.lib') `
        -RunArgs @('--out', $cppJson, '--tier', $Tier, '--allow-pending') `
        -Toolchain $toolchain

    if (-not [string]::IsNullOrWhiteSpace($cppResult.RunOutput)) {
        Write-Host $cppResult.RunOutput
    }
    if ($cppResult.RunExitCode -ne 0) {
        throw "portrait_bridge_tests.exe exited with code $($cppResult.RunExitCode)."
    }

    $cppRecords = ConvertFrom-E2EJsonArray -JsonText (Read-Utf8Text -Path $cppJson)

    # Step 3: Run isolated Vitest F13 TypeScript contract tests
    $tsJson = Join-Path $resolvedBuildDir 'ts_results.json'
    $vitestRawJson = Join-Path $resolvedBuildDir 'vitest_raw.json'
    if (Test-Path -LiteralPath $tsJson -PathType Leaf) {
        Remove-Item -LiteralPath $tsJson -Force
    }
    if (Test-Path -LiteralPath $vitestRawJson -PathType Leaf) {
        Remove-Item -LiteralPath $vitestRawJson -Force
    }

    $expectedTsTests = @(
        @{ id = 'T1-F13-06'; tier = 1; feature = 'F13'; name = 'protocol.ts & MotionStore/Mock M4 normalization' },
        @{ id = 'T1-F13-07'; tier = 1; feature = 'F13'; name = 'MotionStore actors.getPortrait caching/dedup & formation mutations' },
        @{ id = 'T2-F13-06'; tier = 2; feature = 'F13'; name = 'Mock phase1 degradation & MotionWebView2Client wire correlation / stale-sid / invalid swap' },
        @{ id = 'T3-F13-01'; tier = 3; feature = 'F13'; name = 'Pairwise Store x Mock x Validation multi-VMD probeFiles & formation edits' },
        @{ id = 'T4-F13-01'; tier = 4; feature = 'F13'; name = 'Multi-actor group-song Store/Mock lifecycle' }
    )
    if ($Tier -ne 'All') {
        $tierNum = [int]$Tier
        $expectedTsTests = @($expectedTsTests | Where-Object { [int]$_.tier -eq $tierNum })
    }

    $tsRecordsList = New-Object System.Collections.Generic.List[object]

    if (-not $paths.HasFrontendDeps) {
        if ($expectedTsTests.Count -gt 0) {
            $suiteInfraExit = 1
        }
        foreach ($exp in $expectedTsTests) {
            $tsRecordsList.Add([pscustomobject]@{
                id      = $exp.id
                tier    = [int]$exp.tier
                feature = $exp.feature
                name    = $exp.name
                status  = 'FAIL'
                detail  = "Frontend dependencies missing (vitest not found at '$($paths.VitestCmd)')."
            })
        }
    } else {
        $vitestCfg = Join-Path $PSScriptRoot 'vitest.e2e.config.mjs'
        $env:GKE2E_TS_REPORT_JSON = $tsJson
        $env:GKE2E_TIER_FILTER = $Tier

        $oldPref = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $global:LASTEXITCODE = 0
            $vitestOut = & $paths.VitestCmd run --config $vitestCfg --reporter=json --outputFile=$vitestRawJson 2>&1
            $vitestExit = [int]$LASTEXITCODE
        } finally {
            $ErrorActionPreference = $oldPref
        }
        $vitestText = ($vitestOut | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        if (-not [string]::IsNullOrWhiteSpace($vitestText)) {
            Write-Host $vitestText
        }

        $sidecarById = @{}
        $sidecarError = ''
        if ($expectedTsTests.Count -gt 0) {
            if (-not (Test-Path -LiteralPath $tsJson -PathType Leaf)) {
                $sidecarError = "ts_results.json missing at '$tsJson' (vitestExit=$vitestExit)."
            } else {
                try {
                    $rawSidecar = ConvertFrom-E2EJsonArray -JsonText (Read-Utf8Text -Path $tsJson)
                    $requiredSidecarProps = @('id', 'tier', 'feature', 'name', 'status', 'detail')
                    $allowedSidecarStatuses = @('PASS', 'FAIL', 'PENDING_IMPL')
                    foreach ($rec in $rawSidecar) {
                        if ($null -eq $rec) {
                            throw 'ts_results.json contains a null element.'
                        }
                        foreach ($rp in $requiredSidecarProps) {
                            if ($null -eq $rec.PSObject.Properties[$rp]) {
                                throw "ts_results.json record missing required property '$rp'."
                            }
                        }
                        $recId = [string]$rec.id
                        $recStatus = [string]$rec.status
                        if ([string]::IsNullOrWhiteSpace($recId)) {
                            throw 'ts_results.json record has empty id.'
                        }
                        if ($allowedSidecarStatuses -notcontains $recStatus) {
                            throw "ts_results.json record '$recId' has invalid status '$recStatus'."
                        }
                        $sidecarById[$recId] = $rec
                    }
                    if ($sidecarById.Count -eq 0) {
                        $sidecarError = "ts_results.json at '$tsJson' contained 0 records (vitestExit=$vitestExit)."
                    }
                } catch {
                    $sidecarError = "Invalid ts_results.json '$tsJson': $($_.Exception.Message)"
                }
            }
        }

        $vitestAssertionsById = @{}
        $vitestLoadError = ''
        $hasRecordedVitestFailure = $false
        $vitestSuiteHookError = ''
        if (Test-Path -LiteralPath $vitestRawJson -PathType Leaf) {
            try {
                $vObj = ConvertFrom-Json -InputObject ((Read-Utf8Text -Path $vitestRawJson).TrimStart([char]0xFEFF))
                if ($null -eq $vObj -or $null -eq $vObj.PSObject.Properties['testResults'] -or $null -eq $vObj.testResults) {
                    $vitestLoadError = "Vitest raw JSON missing testResults (exit=$vitestExit)."
                } else {
                    foreach ($tr in @($vObj.testResults)) {
                        if ($null -eq $tr) { continue }
                        if ($null -ne $tr.PSObject.Properties['assertionResults'] -and $null -ne $tr.assertionResults) {
                            foreach ($ar in @($tr.assertionResults)) {
                                if ($null -eq $ar) { continue }
                                $arStatus = if ($null -ne $ar.PSObject.Properties['status']) { [string]$ar.status } else { '' }
                                if ($arStatus -eq 'failed') {
                                    $hasRecordedVitestFailure = $true
                                }
                                $title = if ($null -ne $ar.PSObject.Properties['title']) { [string]$ar.title } else { '' }
                                if ($title -match '^\[([^\]]+)\]\s*') {
                                    $tid = $Matches[1]
                                    $vitestAssertionsById[$tid] = $ar
                                }
                            }
                        }
                        $trMsg = if ($null -ne $tr.PSObject.Properties['message']) { [string]$tr.message } else { '' }
                        $trStatus = if ($null -ne $tr.PSObject.Properties['status']) { [string]$tr.status } else { '' }
                        if (-not [string]::IsNullOrWhiteSpace($trMsg)) {
                            $vitestLoadError = $trMsg
                            if ($trStatus -eq 'failed') {
                                $vitestSuiteHookError = $trMsg
                            }
                        } elseif ($trStatus -eq 'failed' -and -not $hasRecordedVitestFailure) {
                            $vitestSuiteHookError = "Vitest test file failed without assertion failure (exit=$vitestExit)."
                        }
                    }
                }
            } catch {
                $vitestLoadError = "Failed parsing Vitest JSON output: $($_.Exception.Message)"
            }
        } else {
            $vitestLoadError = "Vitest raw JSON report missing (exit=$vitestExit)."
        }

        $vitestUnrecordedFailure = ''
        if ($expectedTsTests.Count -gt 0 -and (($vitestExit -ne 0 -and -not $hasRecordedVitestFailure) -or -not [string]::IsNullOrWhiteSpace($vitestSuiteHookError))) {
            $vitestUnrecordedFailure = if (-not [string]::IsNullOrWhiteSpace($vitestSuiteHookError)) {
                "Vitest suite/hook failure (exit=$vitestExit): $vitestSuiteHookError"
            } elseif (-not [string]::IsNullOrWhiteSpace($vitestLoadError)) {
                "Vitest exited with code $vitestExit without a recorded test failure: $vitestLoadError"
            } else {
                "Vitest exited with code $vitestExit without a recorded test failure."
            }
        }

        $tsInfraFailCount = 0
        foreach ($exp in $expectedTsTests) {
            $tid = [string]$exp.id
            $ar = if ($vitestAssertionsById.ContainsKey($tid)) { $vitestAssertionsById[$tid] } else { $null }
            $sc = if ($sidecarById.ContainsKey($tid)) { $sidecarById[$tid] } else { $null }

            if ($null -eq $ar) {
                $tsInfraFailCount++
                $failMsg = if (-not [string]::IsNullOrWhiteSpace($vitestUnrecordedFailure)) {
                    $vitestUnrecordedFailure
                } elseif (-not [string]::IsNullOrWhiteSpace($vitestLoadError)) {
                    $vitestLoadError
                } elseif (-not [string]::IsNullOrWhiteSpace($sidecarError)) {
                    $sidecarError
                } else {
                    "Vitest did not report assertion for '$tid' (exit=$vitestExit)."
                }
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = $failMsg
                })
                continue
            }

            $vStatus = if ($null -ne $ar.PSObject.Properties['status']) { [string]$ar.status } else { '' }
            if ($vStatus -eq 'failed') {
                $msgs = @()
                if ($null -ne $ar.PSObject.Properties['failureMessages'] -and $null -ne $ar.failureMessages) {
                    $msgs = @($ar.failureMessages | ForEach-Object { $_.ToString() })
                }
                $errDetail = if ($msgs.Count -gt 0) {
                    ($msgs -join ' | ')
                } elseif ($null -ne $sc) {
                    [string]$sc.detail
                } else {
                    "Vitest assertion failed for '$tid'."
                }
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = $errDetail
                })
            } elseif (-not [string]::IsNullOrWhiteSpace($sidecarError)) {
                $tsInfraFailCount++
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = $sidecarError
                })
            } elseif (-not [string]::IsNullOrWhiteSpace($vitestUnrecordedFailure)) {
                $tsInfraFailCount++
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = $vitestUnrecordedFailure
                })
            } elseif ($null -eq $sc) {
                $tsInfraFailCount++
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = "Vitest status '$vStatus' without ts_e2e_report sidecar record."
                })
            } elseif ($vStatus -eq 'passed' -and [string]$sc.status -ne 'PASS') {
                $tsInfraFailCount++
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = "Sidecar status '$([string]$sc.status)' mismatches Vitest 'passed'."
                })
            } elseif ($vStatus -eq 'skipped' -and [string]$sc.status -ne 'PENDING_IMPL') {
                $tsInfraFailCount++
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = "Sidecar status '$([string]$sc.status)' mismatches Vitest 'skipped'."
                })
            } elseif ($vStatus -ne 'passed' -and $vStatus -ne 'skipped') {
                $tsInfraFailCount++
                $tsRecordsList.Add([pscustomobject]@{
                    id      = $tid
                    tier    = [int]$exp.tier
                    feature = [string]$exp.feature
                    name    = [string]$exp.name
                    status  = 'FAIL'
                    detail  = "Unexpected Vitest status '$vStatus' for '$tid'."
                })
            } else {
                $tsRecordsList.Add([pscustomobject]@{
                    id      = [string]$sc.id
                    tier    = [int]$sc.tier
                    feature = [string]$sc.feature
                    name    = [string]$sc.name
                    status  = [string]$sc.status
                    detail  = [string]$sc.detail
                })
            }
        }

        if ($expectedTsTests.Count -gt 0 -and (
            -not [string]::IsNullOrWhiteSpace($sidecarError) -or
            -not [string]::IsNullOrWhiteSpace($vitestUnrecordedFailure) -or
            $tsInfraFailCount -gt 0)) {
            $suiteInfraExit = 1
        }
    }

    # Step 4: Combine C++ + TS results and validate schema
    $combined = New-Object System.Collections.Generic.List[object]
    foreach ($rec in $cppRecords) {
        $combined.Add([pscustomobject]@{
            id      = [string]$rec.id
            tier    = [int]$rec.tier
            feature = [string]$rec.feature
            name    = [string]$rec.name
            status  = [string]$rec.status
            detail  = [string]$rec.detail
        })
    }
    foreach ($rec in $tsRecordsList) {
        $combined.Add([pscustomobject]@{
            id      = [string]$rec.id
            tier    = [int]$rec.tier
            feature = [string]$rec.feature
            name    = [string]$rec.name
            status  = [string]$rec.status
            detail  = [string]$rec.detail
        })
    }

    if ($combined.Count -eq 0) {
        throw "No test records produced for Tier='$Tier'."
    }

    $allowedStatuses = @('PASS', 'FAIL', 'PENDING_IMPL')
    foreach ($rec in $combined) {
        if ([string]::IsNullOrWhiteSpace($rec.id) -or
            [string]::IsNullOrWhiteSpace($rec.feature) -or
            [string]::IsNullOrWhiteSpace($rec.name) -or
            ($rec.tier -lt 1 -or $rec.tier -gt 4) -or
            ($allowedStatuses -notcontains $rec.status)) {
            throw "Invalid test record schema for id='$($rec.id)'."
        }
    }

    $jsonArrText = ConvertTo-Json -InputObject @($combined.ToArray()) -Depth 6
    # Validate roundtrip array parse
    $null = ConvertFrom-E2EJsonArray -JsonText $jsonArrText
    Write-Utf8NoBomText -Path $resolvedOutJson -Content ($jsonArrText + "`n")
} finally {
    $env:GKE2E_BUILD_DIR = $prevBuildDirEnv
    $env:GKE2E_OCTO_EXTRACTED_DIR = $prevOctoDirEnv
    $env:GAKUMOTION_GAME_ROOT = $prevGameRootEnv
    $env:GKE2E_TS_REPORT_JSON = $prevTsReportEnv
    $env:GKE2E_TIER_FILTER = $prevTierFilterEnv
}

exit $suiteInfraExit
