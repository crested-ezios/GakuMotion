#Requires -Version 5.1
[CmdletBinding()]
param([switch]$RunTests, [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$gbbRoot = $PSScriptRoot
$gbbMinHookFiles = @('include\MinHook.h', 'src\hook.c', 'src\buffer.c', 'src\trampoline.c', 'src\hde\hde64.c')
foreach ($gbbMinHookFile in $gbbMinHookFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $gbbRoot ('deps\minhook\' + $gbbMinHookFile)) -PathType Leaf)) {
        throw 'Vendored MinHook sources are missing. Restore the complete source snapshot; see deps/minhook/UPSTREAM.md.'
    }
}
$gbbVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $gbbVsWhere)) { throw 'Visual Studio C++ Build Tools and Windows SDK are required.' }
$gbbVs = & $gbbVsWhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $gbbVs) { throw 'No MSVC x64 installation found.' }
$gbbTools = Get-ChildItem -LiteralPath (Join-Path $gbbVs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$gbbSdk = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$gbbSdkVersion = Get-ChildItem -LiteralPath (Join-Path $gbbSdk 'Include') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $gbbTools -or -not $gbbSdkVersion) { throw 'MSVC or Windows SDK headers are missing.' }
$gbbBuild = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $gbbRoot 'build' }
$gbbObj = Join-Path $gbbBuild 'obj'
New-Item -ItemType Directory -Force -Path $gbbObj | Out-Null
$gbbOldPath = $env:PATH; $gbbOldInclude = $env:INCLUDE; $gbbOldLib = $env:LIB
try {
    $env:PATH = (Join-Path $gbbTools.FullName 'bin\Hostx64\x64') + ';' + $env:PATH
    $gbbHeaders = @((Join-Path $gbbTools.FullName 'include'))
    foreach ($gbbPart in @('ucrt','um','shared','winrt','cppwinrt')) { $gbbHeaders += Join-Path $gbbSdkVersion.FullName $gbbPart }
    $env:INCLUDE = $gbbHeaders -join ';'
    $env:LIB = @((Join-Path $gbbTools.FullName 'lib\x64'), (Join-Path $gbbSdk ('Lib\' + $gbbSdkVersion.Name + '\ucrt\x64')), (Join-Path $gbbSdk ('Lib\' + $gbbSdkVersion.Name + '\um\x64'))) -join ';'
    $gbbCommon = @('/nologo','/O2','/MT','/W4','/utf-8','/D_CRT_SECURE_NO_WARNINGS',('/I' + (Join-Path $gbbRoot 'include')),('/I' + (Join-Path $gbbRoot 'deps\minhook\include')))
    $gbbMinHookSources = @('hook.c','buffer.c','trampoline.c','hde\hde64.c') | ForEach-Object { Join-Path $gbbRoot ('deps\minhook\src\' + $_) }
    & cl.exe @gbbCommon /c @gbbMinHookSources "-Fo$gbbObj/"
    if ($LASTEXITCODE -ne 0) { throw 'MinHook compilation failed.' }
    $gbbMinHookObjects = @('hook.obj','buffer.obj','trampoline.obj','hde64.obj') | ForEach-Object { Join-Path $gbbObj $_ }
    $gbbRc = Join-Path $gbbSdk ('bin\' + $gbbSdkVersion.Name + '\x64\rc.exe')
    if (-not (Test-Path -LiteralPath $gbbRc)) { throw 'Windows SDK resource compiler missing.' }
    $gbbRes = Join-Path $gbbObj 'GakuBurstBridge.res'
    & $gbbRc /nologo /fo $gbbRes (Join-Path $gbbRoot 'src\GakuBurstBridge.rc')
    if ($LASTEXITCODE -ne 0) { throw 'Version resource compilation failed.' }
    $gbbDll = Join-Path $gbbBuild 'GakuBurstBridge.dll'
    & cl.exe @gbbCommon /std:c++20 /EHsc /LD /DGBB_BUILD_DLL (Join-Path $gbbRoot 'src\GakuBurstBridge.cpp') @gbbMinHookObjects $gbbRes "-Fo$gbbObj/" "-Fe$gbbDll" /link /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT
    if ($LASTEXITCODE -ne 0) { throw 'GakuBurstBridge compilation failed.' }
    if ($RunTests) {
        & cl.exe @gbbCommon /TC /c (Join-Path $gbbRoot 'tests\abi_c.c') "-Fo$gbbObj/abi_c.obj"
        if ($LASTEXITCODE -ne 0) { throw 'C ABI compilation failed.' }
        $gbbAbiTest = Join-Path $gbbBuild 'abi_tests.exe'
        & cl.exe @gbbCommon /std:c++20 /EHsc (Join-Path $gbbRoot 'tests\abi_tests.cpp') (Join-Path $gbbObj 'abi_c.obj') "-Fo$gbbObj/" "-Fe$gbbAbiTest"
        if ($LASTEXITCODE -ne 0) { throw 'ABI test compilation failed.' }
        & $gbbAbiTest $gbbDll
        if ($LASTEXITCODE -ne 0) { throw 'ABI tests failed.' }
        $gbbCoreTest = Join-Path $gbbBuild 'core_tests.exe'
        & cl.exe @gbbCommon /std:c++20 /EHsc /DGBB_TESTING (Join-Path $gbbRoot 'tests\core_tests.cpp') @gbbMinHookObjects "-Fo$gbbObj/" "-Fe$gbbCoreTest"
        if ($LASTEXITCODE -ne 0) { throw 'Core test compilation failed.' }
        & $gbbCoreTest
        if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
        $gbbQuartzTest = Join-Path $gbbBuild 'quartz_experiment_tests.exe'
        & cl.exe @gbbCommon /std:c++20 /EHsc /DGBB_TESTING (Join-Path $gbbRoot 'tests\quartz_experiment_tests.cpp') @gbbMinHookObjects "-Fo$gbbObj/" "-Fe$gbbQuartzTest"
        if ($LASTEXITCODE -ne 0) { throw 'Quartz experiment test compilation failed.' }
        & $gbbQuartzTest
        if ($LASTEXITCODE -ne 0) { throw 'Quartz experiment tests failed.' }
        $gbbSleeveTest = Join-Path $gbbBuild 'sleeve_muscle_probe_tests.exe'
        & cl.exe @gbbCommon /std:c++20 /EHsc /DGBB_TESTING (Join-Path $gbbRoot 'tests\sleeve_muscle_probe_tests.cpp') @gbbMinHookObjects "-Fo$gbbObj/" "-Fe$gbbSleeveTest"
        if ($LASTEXITCODE -ne 0) { throw 'Sleeve probe test compilation failed.' }
        & $gbbSleeveTest
        if ($LASTEXITCODE -ne 0) { throw 'Sleeve probe tests failed.' }
        $gbbMultiTest = Join-Path $gbbBuild 'multi_context_tests.exe'
        & cl.exe @gbbCommon /std:c++20 /EHsc /DGBB_TESTING (Join-Path $gbbRoot 'tests\multi_context_tests.cpp') @gbbMinHookObjects "-Fo$gbbObj/" "-Fe$gbbMultiTest"
        if ($LASTEXITCODE -ne 0) { throw 'Multi-context test compilation failed.' }
        & $gbbMultiTest
        if ($LASTEXITCODE -ne 0) { throw 'Multi-context tests failed.' }
    }
    Get-FileHash -LiteralPath $gbbDll -Algorithm SHA256
    Write-Host 'Build complete. Deployment is explicit; this script does not modify the game.'
} finally {
    $env:PATH = $gbbOldPath; $env:INCLUDE = $gbbOldInclude; $env:LIB = $gbbOldLib
}
