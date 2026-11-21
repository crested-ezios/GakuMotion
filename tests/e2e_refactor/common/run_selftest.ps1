#Requires -Version 5.1
# NOTE: Keep this script strictly ASCII-only (Windows PowerShell 5.1 ANSI parsing).

[CmdletBinding()]
param(
    [string]$BuildRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'e2e_paths.ps1')

$paths = Get-E2EPaths -BuildRoot $BuildRoot
$toolchain = Resolve-E2EToolchain -RepoRoot $paths.RepoRoot

Write-Host '[Selftest] Toolchain resolved:'
Write-Host "  RepoRoot      : $($paths.RepoRoot)"
Write-Host "  BuildRoot     : $($paths.BuildRoot)"
Write-Host "  MSVC          : $($toolchain.MsvcVersion) ($($toolchain.ClExe))"
Write-Host "  Windows SDK   : $($toolchain.WindowsSdkVer) ($($toolchain.WindowsSdkRoot))"
Write-Host "  DotNet        : $($toolchain.DotnetVersion) (HasDotnet8=$($toolchain.HasDotnet8))"
Write-Host "  Python        : $($toolchain.PythonVersion) ($($toolchain.PythonExe))"
Write-Host "  NativeAOT DLL : $($paths.NativeAotDll) (Exists=$($paths.HasNativeAotDll))"
Write-Host "  Game Octo     : $($paths.OctoManifestPath) (HasCache=$($paths.HasGameOctoCache))"

if (-not $toolchain.HasDotnet8) {
    throw 'Expected .NET 8 SDK via global.json / dotnet --list-sdks.'
}
if (-not $toolchain.PythonExe) {
    throw 'Python 3 executable not found.'
}
if (-not $paths.HasNativeAotDll) {
    throw "NativeAOT DLL not found at '$($paths.NativeAotDll)'."
}

# ---------------------------------------------------------------------------
# Stage 1: Compile & run selftest_common.cpp (/W4 /WX, no src/) and selftest_src_probe.cpp (/W3, src/)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[Selftest 1/4] Compiling & running selftest_common.cpp (/W4 /WX)...'
$commonBuildDir = Join-Path $paths.BuildRoot 'selftest_build'
$commonExe = Join-Path $commonBuildDir 'selftest_common.exe'
$commonReportJson = Join-Path $paths.BuildRoot 'selftest_common_report.json'
$sampleJsonPath = Join-Path $paths.BuildRoot 'selftest_sample.json'
$cppFixturesDir = Join-Path $paths.BuildRoot 'selftest_cpp'

$resCommon = Invoke-E2ECppBuildAndRun `
    -SourceFiles @((Join-Path $paths.CommonDir 'selftest_common.cpp')) `
    -ExePath $commonExe `
    -BuildDir $commonBuildDir `
    -WarningLevel '/W4' `
    -TreatWarningsAsErrors `
    -Defines @('_CRT_SECURE_NO_WARNINGS') `
    -Libs @() `
    -RunArgs @('--out', $commonReportJson) `
    -Toolchain $toolchain

if ($resCommon.RunExitCode -ne 0) {
    throw "selftest_common.exe failed (exit=$($resCommon.RunExitCode)):`n$($resCommon.RunOutput)"
}

# Validate UTF-8 no BOM & parsed schema
$rawCommonBytes = [System.IO.File]::ReadAllBytes($commonReportJson)
if ($rawCommonBytes.Length -ge 3 -and $rawCommonBytes[0] -eq 0xEF -and $rawCommonBytes[1] -eq 0xBB -and $rawCommonBytes[2] -eq 0xBF) {
    throw "selftest_common_report.json must not contain a UTF-8 BOM."
}
$commonEntries = ConvertFrom-E2EJsonArray -JsonText (Read-Utf8Text -Path $commonReportJson)
if ($commonEntries.Count -ne 6) {
    throw "Expected 6 selftest_common results, got $($commonEntries.Count)."
}
foreach ($e in $commonEntries) {
    if ($e.status -ne 'PASS') {
        throw "selftest_common test '$($e.id)' did not PASS: $($e.status) - $($e.detail)"
    }
}

# Emit sample JSON & verify CJK + escaped control char roundtrip
$oldPref = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $global:LASTEXITCODE = 0
    & $commonExe --emit-sample-json $sampleJsonPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "selftest_common.exe --emit-sample-json failed (exit=$LASTEXITCODE)." }
    $global:LASTEXITCODE = 0
    & $commonExe --emit-fixtures $cppFixturesDir | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "selftest_common.exe --emit-fixtures failed (exit=$LASTEXITCODE)." }
} finally {
    $ErrorActionPreference = $oldPref
}
$sampleEntries = ConvertFrom-E2EJsonArray -JsonText (Read-Utf8Text -Path $sampleJsonPath)
if ($sampleEntries.Count -ne 3) {
    throw "Expected 3 sample JSON entries, got $($sampleEntries.Count)."
}
if ($sampleEntries[0].name -ne ([string][char]0x56E2 + [string][char]0x66F2 + ' 1' + [string][char]0x53F7 + [string][char]0x4F4D)) {
    throw "Sample JSON UTF-8 CJK round-trip mismatch: '$($sampleEntries[0].name)'"
}
Write-Host '  selftest_common.cpp (/W4 /WX): PASS (6/6 tests, sample JSON & C++ fixtures emitted).'

Write-Host '[Selftest 1/4] Compiling & running selftest_src_probe.cpp (/W3 against src/)...'
$probeExe = Join-Path $commonBuildDir 'selftest_src_probe.exe'
$probeJson = Join-Path $paths.BuildRoot 'selftest_src_probe.json'
$resProbe = Invoke-E2ECppBuildAndRun `
    -SourceFiles @((Join-Path $paths.CommonDir 'selftest_src_probe.cpp')) `
    -ExePath $probeExe `
    -BuildDir $commonBuildDir `
    -WarningLevel '/W3' `
    -UseSrcDefaults `
    -RunArgs @('--out', $probeJson, '--allow-pending') `
    -Toolchain $toolchain

if ($resProbe.RunExitCode -ne 0) {
    throw "selftest_src_probe.exe failed (exit=$($resProbe.RunExitCode)):`n$($resProbe.RunOutput)"
}
$probeEntries = ConvertFrom-E2EJsonArray -JsonText (Read-Utf8Text -Path $probeJson)
if ($probeEntries.Count -lt 6) {
    throw "Expected >= 6 probe entries, got $($probeEntries.Count)."
}
foreach ($pe in $probeEntries) {
    if ($pe.status -eq 'FAIL') {
        throw "selftest_src_probe entry '$($pe.id)' failed: $($pe.detail)"
    }
    Write-Host ("  Probe {0} [{1}]: {2} ({3})" -f $pe.id, $pe.feature, $pe.status, $pe.detail)
}

# ---------------------------------------------------------------------------
# Stage 2: Python fixture parity & exact SHA-256 comparison
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[Selftest 2/4] Generating Python fixtures and verifying SHA-256 byte parity...'
$pyFixturesDir = Join-Path $paths.BuildRoot 'selftest_py'
if (Test-Path -LiteralPath $pyFixturesDir -PathType Container) {
    Remove-Item -LiteralPath $pyFixturesDir -Recurse -Force
}

$pyGenScript = Join-Path $paths.CommonDir 'vmd_fixture_gen.py'
$pyCmdArgs = @($toolchain.PythonArgsPrefix) + @($pyGenScript, 'emit-parity', '--out-dir', $pyFixturesDir)
$oldPref = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $global:LASTEXITCODE = 0
    $pyOut = & $toolchain.PythonExe @pyCmdArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "vmd_fixture_gen.py failed (exit=$LASTEXITCODE): $pyOut"
    }
} finally {
    $ErrorActionPreference = $oldPref
}

$binaryExts = @('.vmd', '.wav', '.gkclip', '.gkcamera')
$cppBinFiles = @(Get-ChildItem -LiteralPath $cppFixturesDir -Recurse -File |
    Where-Object { $binaryExts -contains $_.Extension.ToLowerInvariant() } |
    Sort-Object -Property FullName)

if ($cppBinFiles.Count -lt 25) {
    throw "Expected >= 25 binary parity files in '$cppFixturesDir', found $($cppBinFiles.Count)."
}

foreach ($cppFile in $cppBinFiles) {
    $relPath = $cppFile.FullName.Substring($cppFixturesDir.Length).TrimStart('\', '/')
    $pyFile = Join-Path $pyFixturesDir $relPath
    if (-not (Test-Path -LiteralPath $pyFile -PathType Leaf)) {
        throw "Missing Python parity file for '$relPath' at '$pyFile'."
    }
    $cppHash = (Get-FileHash -LiteralPath $cppFile.FullName -Algorithm SHA256).Hash
    $pyHash = (Get-FileHash -LiteralPath $pyFile -Algorithm SHA256).Hash
    if ($cppHash -ne $pyHash) {
        throw "SHA-256 parity mismatch for '$relPath': C++=$cppHash vs Python=$pyHash"
    }
}
Write-Host "  SHA-256 byte-for-byte parity verified across $($cppBinFiles.Count) binary files (.vmd/.wav/.gkclip/.gkcamera)."

# ---------------------------------------------------------------------------
# Stage 3: Exercise fixtures against real build/nativeaot/GakuMotion.VmdImporter.Native.dll
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[Selftest 3/4] Exercising NativeAOT GakuMotion.VmdImporter.Native.dll via ctypes...'
$aotWorkDir = Join-Path $paths.BuildRoot 'selftest_aot'
if (Test-Path -LiteralPath $aotWorkDir -PathType Container) {
    Remove-Item -LiteralPath $aotWorkDir -Recurse -Force
}
New-Item -ItemType Directory -Path $aotWorkDir -Force | Out-Null

$aotCheckPy = @'
import ctypes
import json
from pathlib import Path
import sys

dll_path = Path(sys.argv[1])
fixtures_dir = Path(sys.argv[2])
work_dir = Path(sys.argv[3])
motions_root = work_dir / "motions"
motions_root.mkdir(parents=True, exist_ok=True)

mod = ctypes.CDLL(str(dll_path))
abi_fn = mod.GkVmdImporter_AbiVersion
abi_fn.restype = ctypes.c_int32
abi_fn.argtypes = []
abi = abi_fn()
assert abi == 4, f"Expected GkVmdImporter_AbiVersion == 4, got {abi}"

run_fn = mod.GkVmdImport_Run
run_fn.restype = ctypes.c_int32
run_fn.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32]

def invoke_import(req_obj: dict) -> tuple[int, dict]:
    req_bytes = json.dumps(req_obj, ensure_ascii=False).encode("utf-8")
    buf = ctypes.create_string_buffer(65536)
    rc = run_fn(req_bytes, buf, len(buf))
    raw = buf.value.decode("utf-8", errors="replace")
    resp = json.loads(raw) if raw else {}
    return rc, resp

# 1. Positive body + morph + camera + wav import
pos_req = {
    "motion_vmd": str(fixtures_dir / "body_default.vmd"),
    "morph_vmd": str(fixtures_dir / "morph.vmd"),
    "camera_vmd": str(fixtures_dir / "camera.vmd"),
    "audio": str(fixtures_dir / "audio_default.wav"),
    "motions_root": str(motions_root),
    "title": "SelftestPositive"
}
rc, resp = invoke_import(pos_req)
assert rc == 0 and resp.get("ok") is True, f"Positive import failed: rc={rc}, resp={resp}"
out_dir = Path(resp["dir"])
assert (out_dir / "motion.gkclip").is_file(), "Missing motion.gkclip"
assert (out_dir / "camera.gkcamera").is_file(), "Missing camera.gkcamera"
assert (out_dir / "song.wav").is_file(), "Missing song.wav"
assert (out_dir / "info.json").is_file(), "Missing info.json"

# 2. Negative body VMDs (should fail cleanly non-zero)
neg_body_vmds = [
    "eye_only.vmd",
    "empty.vmd",
    "neg_bad_magic.vmd",
    "neg_trunc_header.vmd",
    "neg_trunc_model.vmd",
    "neg_trunc_bone.vmd",
    "neg_inflated_bone.vmd",
    "neg_trunc_morph.vmd",
    "neg_zero_quat.vmd",
]
for name in neg_body_vmds:
    rc_n, resp_n = invoke_import({
        "motion_vmd": str(fixtures_dir / name),
        "motions_root": str(motions_root),
        "title": f"NegBody_{name}"
    })
    assert rc_n != 0 and resp_n.get("ok") is False, f"Expected failure for {name}, got rc={rc_n}, resp={resp_n}"

# 3. Negative camera VMDs
for name in ["neg_trunc_camera.vmd", "neg_zero_fov.vmd", "neg_bad_cam_curve.vmd"]:
    rc_c, resp_c = invoke_import({
        "motion_vmd": str(fixtures_dir / "body_default.vmd"),
        "camera_vmd": str(fixtures_dir / name),
        "motions_root": str(motions_root),
        "title": f"NegCam_{name}"
    })
    assert rc_c != 0 and resp_c.get("ok") is False, f"Expected camera failure for {name}, got rc={rc_c}, resp={resp_c}"

# 4. Negative WAVs
for name in ["neg_trunc_riff.wav", "neg_missing_data.wav", "neg_bad_codec.wav", "neg_zero_data.wav"]:
    rc_w, resp_w = invoke_import({
        "motion_vmd": str(fixtures_dir / "body_default.vmd"),
        "audio": str(fixtures_dir / name),
        "motions_root": str(motions_root),
        "title": f"NegWav_{name}"
    })
    assert rc_w != 0 and resp_w.get("ok") is False, f"Expected audio failure for {name}, got rc={rc_w}, resp={resp_w}"

print("NativeAOT smoke check passed: 1 positive + 16 negative cases verified.")
'@

$aotPyPath = Join-Path $aotWorkDir 'run_aot_smoke.py'
Write-Utf8NoBomText -Path $aotPyPath -Content $aotCheckPy
$aotCmdArgs = @($toolchain.PythonArgsPrefix) + @($aotPyPath, $paths.NativeAotDll, $pyFixturesDir, $aotWorkDir)
$oldPref = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $global:LASTEXITCODE = 0
    $aotOut = & $toolchain.PythonExe @aotCmdArgs 2>&1
    $aotExit = [int]$LASTEXITCODE
} finally {
    $ErrorActionPreference = $oldPref
}
Write-Host ("  " + (($aotOut | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine))
if ($aotExit -ne 0) {
    throw "NativeAOT smoke check failed (exit=$aotExit)."
}

# ---------------------------------------------------------------------------
# Stage 4: Test run_e2e_suite.ps1 against isolated mock domain suites
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[Selftest 4/4] Testing run_e2e_suite.ps1 against isolated mock suites...'
$mockRoot = Join-Path $paths.BuildRoot 'selftest_suites'
if (Test-Path -LiteralPath $mockRoot -PathType Container) {
    Remove-Item -LiteralPath $mockRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $mockRoot -Force | Out-Null

$runnerScript = Join-Path $paths.TestsRoot 'run_e2e_suite.ps1'

function New-MockSuite {
    param(
        [string]$SuiteRoot,
        [string]$DomainName,
        [string]$ScriptBody
    )
    $dir = Join-Path $SuiteRoot $DomainName
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Write-Utf8NoBomText -Path (Join-Path $dir 'run_suite.ps1') -Content $ScriptBody
}

function Invoke-RunnerScenario {
    param(
        [string]$SuitesDir,
        [string]$BuildDir,
        [switch]$AllowPendingMode
    )
    $psExe = (Get-Command 'powershell.exe').Source
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $runnerScript, '-Tier', 'All', '-SuitesRoot', $SuitesDir, '-BuildRoot', $BuildDir)
    if ($AllowPendingMode) {
        $args += '-AllowPending'
    }
    $oldPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $out = & $psExe @args 2>&1
        $rc = [int]$LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPref
    }
    return [pscustomobject]@{
        ExitCode = $rc
        Output   = ($out | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    }
}

# Fake common/run_suite.ps1 MUST be excluded by discovery
New-MockSuite -SuiteRoot $mockRoot -DomainName 'common' -ScriptBody @'
param([string]$Tier='All', [string]$OutJson='', [string]$BuildDir='')
throw 'common directory should never be executed as a domain suite!'
'@

# Scenario A: All PASS
$scenADir = Join-Path $mockRoot 'scenA'
New-MockSuite -SuiteRoot $scenADir -DomainName 'common' -ScriptBody @'
param([string]$Tier='All', [string]$OutJson='', [string]$BuildDir='')
throw 'common directory must be ignored!'
'@
New-MockSuite -SuiteRoot $scenADir -DomainName 'alpha' -ScriptBody @'
param([string]$Tier='All', [string]$OutJson='', [string]$BuildDir='')
$json = '[{"id":"T1-F2-01","tier":1,"feature":"F2","name":"Alpha pass","status":"PASS","detail":""}]'
[IO.File]::WriteAllText($OutJson, $json, (New-Object System.Text.UTF8Encoding $false))
exit 0
'@
New-MockSuite -SuiteRoot $scenADir -DomainName 'beta' -ScriptBody @'
param([string]$Tier='All', [string]$OutJson='', [string]$BuildDir='')
$json = '[{"id":"T2-F5-01","tier":2,"feature":"F5","name":"Beta pass","status":"PASS","detail":""}]'
[IO.File]::WriteAllText($OutJson, $json, (New-Object System.Text.UTF8Encoding $false))
exit 0
'@
$buildA = Join-Path $mockRoot 'buildA'
$resA = Invoke-RunnerScenario -SuitesDir $scenADir -BuildDir $buildA
if ($resA.ExitCode -ne 0) {
    throw "Scenario A (all PASS strict) expected exit 0, got $($resA.ExitCode):`n$($resA.Output)"
}
$sumABytes = [System.IO.File]::ReadAllBytes((Join-Path $buildA 'e2e_summary.json'))
if ($sumABytes.Length -ge 3 -and $sumABytes[0] -eq 0xEF -and $sumABytes[1] -eq 0xBB -and $sumABytes[2] -eq 0xBF) {
    throw 'e2e_summary.json must be UTF-8 without BOM.'
}
$sumA = (Read-Utf8Text -Path (Join-Path $buildA 'e2e_summary.json')) | ConvertFrom-Json
if ($sumA.totals.pass -ne 2 -or $sumA.totals.fail -ne 0 -or $sumA.totals.pending_impl -ne 0) {
    throw "Scenario A totals mismatch."
}

# Scenario B: PASS + PENDING_IMPL
$scenBDir = Join-Path $mockRoot 'scenB'
New-MockSuite -SuiteRoot $scenBDir -DomainName 'alpha' -ScriptBody @'
param([string]$Tier='All', [string]$OutJson='', [string]$BuildDir='')
$json = '[{"id":"T1-F2-01","tier":1,"feature":"F2","name":"Alpha pass","status":"PASS","detail":""},{"id":"T1-F5-01","tier":1,"feature":"F5","name":"Alpha pend","status":"PENDING_IMPL","detail":"waiting M2"}]'
[IO.File]::WriteAllText($OutJson, $json, (New-Object System.Text.UTF8Encoding $false))
exit 0
'@
$resBStrict = Invoke-RunnerScenario -SuitesDir $scenBDir -BuildDir (Join-Path $mockRoot 'buildB_strict')
if ($resBStrict.ExitCode -ne 1) {
    throw "Scenario B strict expected exit 1 when PENDING_IMPL > 0, got $($resBStrict.ExitCode)."
}
$resBAllow = Invoke-RunnerScenario -SuitesDir $scenBDir -BuildDir (Join-Path $mockRoot 'buildB_allow') -AllowPendingMode
if ($resBAllow.ExitCode -ne 0) {
    throw "Scenario B -AllowPending expected exit 0, got $($resBAllow.ExitCode):`n$($resBAllow.Output)"
}

# Scenario C: FAIL present -> strict and AllowPending both exit 1
$scenCDir = Join-Path $mockRoot 'scenC'
New-MockSuite -SuiteRoot $scenCDir -DomainName 'alpha' -ScriptBody @'
param([string]$Tier='All', [string]$OutJson='', [string]$BuildDir='')
$json = '[{"id":"T1-F2-01","tier":1,"feature":"F2","name":"Alpha fail","status":"FAIL","detail":"boom"},{"id":"T1-F5-01","tier":1,"feature":"F5","name":"Alpha pend","status":"PENDING_IMPL","detail":"waiting"}]'
[IO.File]::WriteAllText($OutJson, $json, (New-Object System.Text.UTF8Encoding $false))
exit 0
'@
$resCStrict = Invoke-RunnerScenario -SuitesDir $scenCDir -BuildDir (Join-Path $mockRoot 'buildC_strict')
$resCAllow = Invoke-RunnerScenario -SuitesDir $scenCDir -BuildDir (Join-Path $mockRoot 'buildC_allow') -AllowPendingMode
if ($resCStrict.ExitCode -ne 1 -or $resCAllow.ExitCode -ne 1) {
    throw "Scenario C expected exit 1 in both strict and -AllowPending modes."
}

# Scenario D: Malformed JSON / non-zero exit -> synthetic FAIL and exit 1
$scenDDir = Join-Path $mockRoot 'scenD'
New-MockSuite -SuiteRoot $scenDDir -DomainName 'broken' -ScriptBody @'
param([string]$Tier='All', [string]$OutJson='', [string]$BuildDir='')
[IO.File]::WriteAllText($OutJson, '{not valid json', (New-Object System.Text.UTF8Encoding $false))
exit 2
'@
$buildD = Join-Path $mockRoot 'buildD'
$resD = Invoke-RunnerScenario -SuitesDir $scenDDir -BuildDir $buildD -AllowPendingMode
if ($resD.ExitCode -ne 1) {
    throw "Scenario D expected exit 1 even with -AllowPending, got $($resD.ExitCode)."
}
$sumD = (Read-Utf8Text -Path (Join-Path $buildD 'e2e_summary.json')) | ConvertFrom-Json
if ($sumD.suite_errors -lt 1 -or $sumD.totals.fail -lt 1) {
    throw "Scenario D expected synthetic suite_errors and FAIL record."
}

Write-Host '  run_e2e_suite.ps1 mock suite checks (common exclusion, strict, -AllowPending, FAIL, broken JSON): PASS.'
Write-Host ''
Write-Host '[Selftest] ALL E2E-1 SELFTEST STAGES PASSED.'
exit 0
