#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# Build local binaries, frontend assets, and synthetic tests. There is no deployment path.
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
[CmdletBinding()]
param(
    [ValidateSet('Preflight', 'Contracts', 'Runtime', 'NativeAOT', 'Frontend', 'All')]
    [string]$Target = 'All',
    [string]$OutputRoot = '',
    [string]$WindowsSdkRoot = '',
    [string]$PortableVisualStudio = '',
    [string]$DotNet = 'dotnet',
    [ValidateSet('Auto', 'Ci', 'Install', 'Skip')]
    [string]$FrontendDependencies = 'Auto',
    [switch]$LegacyImGui
)
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repo 'build'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$runtimeInputs = @('GakuMotion.vcxproj', 'src/GakuMotionPlugin.cpp',
    'deps/KineTexture/src/KineTexture/Math.hpp')
if ($LegacyImGui) {
    $runtimeInputs += 'deps/imgui/imgui.cpp'
}
$importInputs = @('interop/GakuMotion.VmdImporter.Native/GakuMotion.VmdImporter.Native.csproj',
    'candidate/csharp/src/GakuMotion.VmdImporter/GakuMotion.VmdImporter.csproj',
    'candidate/csharp/src/GakuMotion.VmdImporter/Resources/gakumas_bone_profile.json',
    'candidate/csharp/src/GakuMotion.VmdImporter/Resources/mmd_source_skeleton.json',
    'tools/vmd2gkmotion/gakumas_morph_map.json', 'tools/vmd2gkmotion/vmd_eye_profile.json',
    'tools/vmd2gkmotion/tooth_appearance_lut.json', 'tools/vmd2gkmotion/root_retarget_profiles.json',
    'tools/vmd2gkmotion/miku_raw_eye_keys.json')
$frontendInputs = @('frontend/package.json', 'frontend/package-lock.json',
    'frontend/vite.config.ts', 'frontend/build.ps1', 'frontend/index.html', 'frontend/src/App.tsx')
$inputs = switch ($Target) {
    'Runtime' { $runtimeInputs }
    'NativeAOT' { $importInputs }
    'Frontend' { $frontendInputs }
    'Contracts' { @('tests/test_stage_load_runtime.cpp') }
    default { $runtimeInputs + $importInputs + $frontendInputs }
}
foreach ($inputPath in $inputs) {
    if (-not (Test-Path -LiteralPath (Join-Path $repo $inputPath) -PathType Leaf)) { throw ('Missing build input: ' + $inputPath) }
}
if ($Target -in @('NativeAOT', 'All', 'Preflight') -and -not (Get-Command $DotNet -ErrorAction SilentlyContinue)) {
    throw 'Install the .NET 8 SDK, or pass -DotNet to a private SDK installation.'
}
if ($Target -in @('Frontend', 'All', 'Preflight') -and -not (Get-Command npm.cmd -ErrorAction SilentlyContinue)) {
    throw 'Install Node.js/npm and make npm.cmd available on PATH.'
}

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$vs = $PortableVisualStudio
if ([string]::IsNullOrWhiteSpace($vs)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with Desktop development with C++.' }
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ([string]::IsNullOrWhiteSpace($vs)) { throw 'MSVC x64 toolset not found.' }
$msvc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvc) { throw 'MSVC compiler directory not found.' }
$sdk = $WindowsSdkRoot
if ([string]::IsNullOrWhiteSpace($sdk)) {
    $sdk = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
}
if ([string]::IsNullOrWhiteSpace($sdk)) {
    $sdk = @(Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'; Join-Path $env:ProgramFiles 'Windows Kits\10') |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Include') } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($sdk)) { throw 'Windows SDK not found; pass -WindowsSdkRoot.' }
$sdkVersion = Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory |
    Where-Object {
        $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt\stddef.h') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $sdk "Lib\$($_.Name)\um\x64\advapi32.lib") -PathType Leaf)
    } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdkVersion) { throw 'Complete Windows SDK (ucrt headers and um/x64 libs) not found.' }
if ($Target -eq 'Preflight') { Write-Host 'Build inputs, MSVC, Windows SDK, .NET, and Node/npm found.'; exit 0 }
if ($Target -eq 'Runtime' -or $Target -eq 'All') {
    if ($PortableVisualStudio) {
        $env:INCLUDE = @((Join-Path $msvc.FullName 'include'), (Join-Path $sdk "Include\$($sdkVersion.Name)\ucrt"),
            (Join-Path $sdk "Include\$($sdkVersion.Name)\um"), (Join-Path $sdk "Include\$($sdkVersion.Name)\shared")) -join ';'
        $env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), (Join-Path $sdk "Lib\$($sdkVersion.Name)\ucrt\x64"),
            (Join-Path $sdk "Lib\$($sdkVersion.Name)\um\x64")) -join ';'
        $env:PATH = (Join-Path $msvc.FullName 'bin\Hostx64\x64') + ';' + (Join-Path $sdk "bin\$($sdkVersion.Name)\x64") + ';' + $env:PATH
        $nativeOutput = Join-Path $OutputRoot 'native'
        $nativeObjects = Join-Path $OutputRoot 'obj\native'
        New-Item -ItemType Directory -Path $nativeOutput,$nativeObjects -Force | Out-Null
        $compiler = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
        $sources = @('src/GakuMotionPlugin.cpp', 'include/minhook/src/buffer.c',
            'include/minhook/src/hook.c', 'include/minhook/src/trampoline.c',
            'include/minhook/src/hde/hde32.c', 'include/minhook/src/hde/hde64.c')
        $imguiArgs = @()
        if ($LegacyImGui) {
            $sources += @('deps/imgui/imgui.cpp', 'deps/imgui/imgui_draw.cpp',
                'deps/imgui/imgui_widgets.cpp', 'deps/imgui/imgui_tables.cpp')
            $imguiArgs += @('/DGKMOTION_ENABLE_IMGUI', "/I$(Join-Path $repo 'deps/imgui')")
        }
        $sources = $sources | ForEach-Object { Join-Path $repo $_ }
        $args = @('/nologo', '/LD', '/std:c++20', '/utf-8', '/O2', '/EHsc', '/MT', '/W3',
            '/DWIN32', '/DNDEBUG', '/D_WINDOWS', '/D_USRDLL',
            "/I$(Join-Path $repo 'include')", "/I$(Join-Path $repo 'src')",
            "/I$(Join-Path $repo 'deps/KineTexture/src')") + $imguiArgs + @(
            "/Fo$nativeObjects\") + $sources + @('winmm.lib', 'user32.lib', 'comdlg32.lib', 'windowscodecs.lib', 'ole32.lib', 'bcrypt.lib',
            '/link', "/OUT:$(Join-Path $nativeOutput 'GakuMotion.dll')", '/SUBSYSTEM:WINDOWS', '/OPT:REF', '/OPT:ICF')
        & $compiler @args
        if ($LASTEXITCODE -ne 0) { throw ('Portable runtime build failed: ' + $LASTEXITCODE) }
    } else {
        # Some hosts expose PATH / Path / path as three separate process environment
        # keys. MSBuild's ToolTask copies them into a case-insensitive dictionary and
        # throws MSB6001 ("an item with the same key has already been added") before
        # CL runs. Collapse them to one entry so the toolchain starts at all.
        $pathValue = (($env:Path -split ';') | Where-Object { $_ } | Select-Object -Unique) -join ';'
        foreach ($key in @('PATH', 'Path', 'path')) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
        $env:Path = $pathValue
        $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
        $toolset = if ([IO.File]::Exists((Join-Path $vs 'MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\v145\Toolset.props'))) { 'v145' } else { 'v143' }
        $nativeOutput = Join-Path $OutputRoot 'native'
        $nativeObjects = Join-Path $OutputRoot 'obj\native'
        New-Item -ItemType Directory -Path $nativeOutput,$nativeObjects -Force | Out-Null
        $configName = if ($LegacyImGui) { 'Release ImGui' } else { 'Release' }
        # Trailing separators are forward slashes on purpose: PowerShell re-quotes an
        # argument that contains a space (e.g. a checkout under "D:\... \Gakumas Develop\...")
        # and a trailing backslash then escapes the closing quote, so MSBuild sees a
        # literal quote and reports MSB1008 "only one project can be specified".
        & $msbuild (Join-Path $repo 'GakuMotion.vcxproj') "/p:Configuration=$configName" '/p:Platform=x64' "/p:PlatformToolset=$toolset" `
            "/p:OutDir=$nativeOutput/" "/p:IntDir=$nativeObjects/" "/p:ImGuiIncludeDir=$(Join-Path $repo 'deps\imgui')" `
            "/p:KineTextureIncludeDir=$(Join-Path $repo 'deps\KineTexture\src')" '/p:GkCaptureDiagnostics=false' '/nologo' '/verbosity:minimal'
        if ($LASTEXITCODE -ne 0) { throw ('Runtime build failed: ' + $LASTEXITCODE) }
    }
}
if ($Target -eq 'NativeAOT' -or $Target -eq 'All') {
    $aotOutput = Join-Path $OutputRoot 'nativeaot'
    $aotObjects = Join-Path $OutputRoot 'obj\nativeaot'
    New-Item -ItemType Directory -Path $aotOutput,$aotObjects -Force | Out-Null
    $linker = Join-Path $msvc.FullName 'bin\Hostx64\x64\link.exe'
    $libCreator = Join-Path $msvc.FullName 'bin\Hostx64\x64\lib.exe'
    $prevLib = $env:LIB
    $env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), (Join-Path $sdk "Lib\$($sdkVersion.Name)\ucrt\x64"),
        (Join-Path $sdk "Lib\$($sdkVersion.Name)\um\x64")) -join ';'
    Push-Location -LiteralPath $repo
    try {
        & $DotNet publish (Join-Path $repo 'interop\GakuMotion.VmdImporter.Native\GakuMotion.VmdImporter.Native.csproj') `
            -c Release -r win-x64 -o $aotOutput "-p:ArtifactsPath=$aotObjects" -p:IlcUseEnvironmentalTools=true `
            "-p:CppLinker=$linker" "-p:CppLibCreator=$libCreator"
        if ($LASTEXITCODE -ne 0) { throw ('NativeAOT build failed: ' + $LASTEXITCODE) }
    } finally {
        $env:LIB = $prevLib
        Pop-Location
    }
}
if ($Target -eq 'Frontend' -or $Target -eq 'All') {
    $resolvedDeps = $FrontendDependencies
    if ($resolvedDeps -eq 'Auto') {
        $viteBin = Join-Path $repo 'frontend\node_modules\.bin\vite.cmd'
        $resolvedDeps = if (Test-Path -LiteralPath $viteBin -PathType Leaf) { 'Skip' } else { 'Ci' }
    }
    $uiStageDir = Join-Path $OutputRoot 'native\GakuMotion.UI'
    $global:LASTEXITCODE = 0
    & (Join-Path $repo 'frontend\build.ps1') -Dependencies $resolvedDeps -StageDir $uiStageDir -AllowedStageRoot $OutputRoot
    if ($LASTEXITCODE -ne 0) { throw ('Frontend build failed: ' + $LASTEXITCODE) }
}
if ($Target -ne 'Contracts') { Write-Host 'Requested product build completed locally.'; exit 0 }
# The CL/LIB/PATH defaults are only needed by the manual contracts compile below.
# MSBuild resolves the VC toolset itself, and mutating PATH beforehand breaks
# ToolTask on hosts that expose Path/PATH/path as separate environment keys
# ("An item with the same key has already been added", MSB6001).
$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), (Join-Path $sdk "Include\$($sdkVersion.Name)\ucrt"),
    (Join-Path $sdk "Include\$($sdkVersion.Name)\um"), (Join-Path $sdk "Include\$($sdkVersion.Name)\shared")) -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), (Join-Path $sdk "Lib\$($sdkVersion.Name)\ucrt\x64"),
    (Join-Path $sdk "Lib\$($sdkVersion.Name)\um\x64")) -join ';'
$env:PATH = (Join-Path $msvc.FullName 'bin\Hostx64\x64') + ';' + (Join-Path $sdk "bin\$($sdkVersion.Name)\x64") + ';' + $env:PATH
$compiler = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$exe = Join-Path $OutputRoot 'stage_runtime_contracts.exe'
$obj = Join-Path $OutputRoot 'stage_runtime_contracts.obj'
$argsForCompiler = @('/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/W4',
    "/I$(Join-Path $repo 'src')", "/I$(Join-Path $repo 'include')",
    (Join-Path $repo 'tests\test_stage_load_runtime.cpp'), "/Fe:$exe", "/Fo:$obj")
Push-Location -LiteralPath $OutputRoot
try {
    & $compiler @argsForCompiler
    if ($LASTEXITCODE -ne 0) { throw ('Synthetic contract build failed: ' + $LASTEXITCODE) }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw ('Synthetic contract tests failed: ' + $LASTEXITCODE) }
} finally { Pop-Location }
Write-Host 'Synthetic contracts passed.'
