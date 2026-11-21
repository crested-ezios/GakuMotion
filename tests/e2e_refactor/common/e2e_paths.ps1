#Requires -Version 5.1
# NOTE: Keep this script strictly ASCII-only (Windows PowerShell 5.1 ANSI parsing).

Set-StrictMode -Version Latest

function Get-E2ERepoRoot {
    [CmdletBinding()]
    param()

    $candidate = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
    if (-not (Test-Path -LiteralPath (Join-Path $candidate 'src') -PathType Container)) {
        throw "Failed to locate GakuMotion repository root from '$PSScriptRoot' (resolved '$candidate')."
    }
    return $candidate
}

function Write-Utf8NoBomText {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $parent = [System.IO.Path]::GetDirectoryName($fullPath)
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($fullPath, $Content, $utf8NoBom)
}

function Read-Utf8Text {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "File not found: '$Path'"
    }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function ConvertFrom-E2EJsonArray {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$JsonText
    )

    $trimmed = $JsonText.TrimStart([char]0xFEFF).Trim()
    if (-not $trimmed.StartsWith('[')) {
        throw 'JSON root must be a JSON array [...].'
    }
    $parsed = ConvertFrom-Json -InputObject $trimmed
    $items = New-Object System.Collections.Generic.List[object]
    if ($null -ne $parsed) {
        foreach ($item in $parsed) {
            $items.Add($item)
        }
    }
    return ,$items.ToArray()
}

function Get-E2EPaths {
    [CmdletBinding()]
    param(
        [string]$RepoRoot = '',
        [string]$BuildRoot = ''
    )

    if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
        $RepoRoot = Get-E2ERepoRoot
    } else {
        $RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
    }

    if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
        $resolvedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot 'build\e2e_refactor'))
    } else {
        $resolvedBuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
    }

    if (-not (Test-Path -LiteralPath $resolvedBuildRoot -PathType Container)) {
        New-Item -ItemType Directory -Path $resolvedBuildRoot -Force | Out-Null
    }

    $testsRoot = Join-Path $RepoRoot 'tests\e2e_refactor'
    $commonDir = Join-Path $testsRoot 'common'
    $srcDir = Join-Path $RepoRoot 'src'
    $includeDir = Join-Path $RepoRoot 'include'
    $kineTextureSrcDir = Join-Path $RepoRoot 'deps\KineTexture\src'
    $imGuiDir = Join-Path $RepoRoot 'deps\imgui'
    $nativeAotDir = Join-Path $RepoRoot 'build\nativeaot'
    $nativeAotDll = Join-Path $nativeAotDir 'GakuMotion.VmdImporter.Native.dll'

    $frontendDir = Join-Path $RepoRoot 'frontend'
    $frontendBinDir = Join-Path $frontendDir 'node_modules\.bin'
    $vitestCmd = Join-Path $frontendBinDir 'vitest.cmd'
    $viteCmd = Join-Path $frontendBinDir 'vite.cmd'
    $tscCmd = Join-Path $frontendBinDir 'tsc.cmd'

    $gameRoot = if (-not [string]::IsNullOrWhiteSpace($env:GAKUMOTION_GAME_ROOT)) {
        [System.IO.Path]::GetFullPath($env:GAKUMOTION_GAME_ROOT)
    } else {
        'f:\Games\gakumas'
    }
    $octoManifestPath = Join-Path $gameRoot 'octo\pdb\400\705100\octocacheevai'
    $hasGameOctoCache = (Test-Path -LiteralPath $gameRoot -PathType Container) -and
                        (Test-Path -LiteralPath $octoManifestPath -PathType Leaf)

    return [pscustomobject]@{
        RepoRoot          = $RepoRoot
        TestsRoot         = $testsRoot
        CommonDir         = $commonDir
        BuildRoot         = $resolvedBuildRoot
        SrcDir            = $srcDir
        IncludeDir        = $includeDir
        KineTextureSrcDir = $kineTextureSrcDir
        ImGuiDir          = $imGuiDir
        CppIncludeDirs    = @($srcDir, $includeDir, $kineTextureSrcDir, $commonDir)
        NativeAotDir      = $nativeAotDir
        NativeAotDll      = $nativeAotDll
        HasNativeAotDll   = [bool](Test-Path -LiteralPath $nativeAotDll -PathType Leaf)
        FrontendDir       = $frontendDir
        FrontendBinDir    = $frontendBinDir
        VitestCmd         = $vitestCmd
        ViteCmd           = $viteCmd
        TscCmd            = $tscCmd
        HasFrontendDeps   = [bool]((Test-Path -LiteralPath $vitestCmd -PathType Leaf) -and (Test-Path -LiteralPath $viteCmd -PathType Leaf))
        GameRoot          = $gameRoot
        OctoManifestPath  = $octoManifestPath
        HasGameOctoCache  = [bool]$hasGameOctoCache
    }
}

function Resolve-E2EToolchain {
    [CmdletBinding()]
    param(
        [string]$RepoRoot = '',
        [string]$VisualStudioPath = '',
        [string]$WindowsSdkRoot = ''
    )

    if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
        $RepoRoot = Get-E2ERepoRoot
    } else {
        $RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
    }

    # 1. Locate Visual Studio + MSVC toolset
    $vsInstallPath = $VisualStudioPath
    if ([string]::IsNullOrWhiteSpace($vsInstallPath)) {
        $vswhereCandidates = @(
            (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
            (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\Installer\vswhere.exe')
        )
        $vswhere = $vswhereCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
        if (-not $vswhere) {
            throw 'vswhere.exe was not found.'
        }
        $vsInstallPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsInstallPath -is [System.Array]) {
            $vsInstallPath = $vsInstallPath | Select-Object -First 1
        }
    }
    if ([string]::IsNullOrWhiteSpace($vsInstallPath) -or -not (Test-Path -LiteralPath $vsInstallPath -PathType Container)) {
        throw 'Visual Studio installation with VC x64 tools was not found.'
    }

    $msvcToolsRoot = Join-Path $vsInstallPath 'VC\Tools\MSVC'
    if (-not (Test-Path -LiteralPath $msvcToolsRoot -PathType Container)) {
        throw "MSVC tools root not found under '$msvcToolsRoot'."
    }
    $msvcToolsetDir = Get-ChildItem -LiteralPath $msvcToolsRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if (-not $msvcToolsetDir) {
        throw "No MSVC toolset directories found under '$msvcToolsRoot'."
    }
    $msvcRoot = $msvcToolsetDir.FullName
    $msvcBinDir = Join-Path $msvcRoot 'bin\Hostx64\x64'
    $clExe = Join-Path $msvcBinDir 'cl.exe'
    $linkExe = Join-Path $msvcBinDir 'link.exe'
    $libExe = Join-Path $msvcBinDir 'lib.exe'
    $dumpbinExe = Join-Path $msvcBinDir 'dumpbin.exe'
    if (-not (Test-Path -LiteralPath $clExe -PathType Leaf)) {
        throw "cl.exe not found at '$clExe'."
    }

    # 2. Locate Windows SDK 10 (Registry KitsRoot10 + fallback candidates)
    $regKitsRoot = $null
    $regProps = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
    if ($null -ne $regProps -and $null -ne $regProps.PSObject.Properties['KitsRoot10']) {
        $regKitsRoot = [string]$regProps.KitsRoot10
    }

    $sdkCandidates = @(
        $WindowsSdkRoot,
        $regKitsRoot,
        'D:\Windows Kits\10',
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'),
        (Join-Path ${env:ProgramFiles} 'Windows Kits\10')
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_.TrimEnd('\', '/') } |
        Select-Object -Unique

    $sdkRoot = $null
    $sdkVersion = $null
    foreach ($candidate in $sdkCandidates) {
        $incDir = Join-Path $candidate 'Include'
        $libDir = Join-Path $candidate 'Lib'
        if (-not (Test-Path -LiteralPath $incDir -PathType Container) -or -not (Test-Path -LiteralPath $libDir -PathType Container)) {
            continue
        }
        $versions = @(Get-ChildItem -LiteralPath $incDir -Directory |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending)
        foreach ($v in $versions) {
            $stddef = Join-Path $candidate "Include\$($v.Name)\ucrt\stddef.h"
            $advapi = Join-Path $candidate "Lib\$($v.Name)\um\x64\advapi32.lib"
            if ((Test-Path -LiteralPath $stddef -PathType Leaf) -and (Test-Path -LiteralPath $advapi -PathType Leaf)) {
                $sdkRoot = $candidate
                $sdkVersion = $v.Name
                break
            }
        }
        if ($sdkRoot) { break }
    }
    if (-not $sdkRoot -or -not $sdkVersion) {
        throw 'Valid Windows 10/11 SDK with ucrt\stddef.h and um\x64\advapi32.lib was not found.'
    }

    # 3. Locate .NET 8 SDK (check inside $RepoRoot for global.json honoring + dotnet --list-sdks)
    $dotnetCmd = Get-Command 'dotnet' -ErrorAction SilentlyContinue
    $dotnetExe = if ($dotnetCmd) { $dotnetCmd.Source } else { $null }
    $dotnetVersion = $null
    $hasDotnet8 = $false
    if ($dotnetExe) {
        Push-Location -LiteralPath $RepoRoot
        try {
            $oldPref = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $global:LASTEXITCODE = 0
                $verOut = & $dotnetExe --version 2>&1
                if ($LASTEXITCODE -eq 0 -and $verOut) {
                    $dotnetVersion = ($verOut | Select-Object -First 1).ToString().Trim()
                }
                $global:LASTEXITCODE = 0
                $sdksOut = @(& $dotnetExe --list-sdks 2>&1)
                if ($LASTEXITCODE -eq 0) {
                    foreach ($line in $sdksOut) {
                        if ($line.ToString().Trim() -match '^8\.') {
                            $hasDotnet8 = $true
                            break
                        }
                    }
                }
            } finally {
                $ErrorActionPreference = $oldPref
            }
        } finally {
            Pop-Location
        }
        if ($dotnetVersion -and ($dotnetVersion -match '^8\.')) {
            $hasDotnet8 = $true
        }
    }

    # 4. Locate Python >= 3.8
    $pythonExe = $null
    $pythonArgsPrefix = @()
    $pythonVersion = $null
    $pyCmd = Get-Command 'python' -ErrorAction SilentlyContinue
    if ($pyCmd) {
        $pythonExe = $pyCmd.Source
    } else {
        $pyLauncher = Get-Command 'py' -ErrorAction SilentlyContinue
        if ($pyLauncher) {
            $pythonExe = $pyLauncher.Source
            $pythonArgsPrefix = @('-3')
        }
    }
    if ($pythonExe) {
        $oldPref = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $global:LASTEXITCODE = 0
            $pyVerArgs = @($pythonArgsPrefix) + @('--version')
            $pyOut = & $pythonExe @pyVerArgs 2>&1
            if ($LASTEXITCODE -eq 0 -and $pyOut) {
                $pythonVersion = ($pyOut | Select-Object -First 1).ToString().Trim()
            }
        } finally {
            $ErrorActionPreference = $oldPref
        }
    }

    # 5. Locate Node / npm / frontend bins
    $nodeCmd = Get-Command 'node' -ErrorAction SilentlyContinue
    $npmCmd = Get-Command 'npm.cmd' -ErrorAction SilentlyContinue
    if (-not $npmCmd) {
        $npmCmd = Get-Command 'npm' -ErrorAction SilentlyContinue
    }
    $paths = Get-E2EPaths -RepoRoot $RepoRoot

    return [pscustomobject]@{
        VisualStudioPath = $vsInstallPath
        MsvcVersion      = $msvcToolsetDir.Name
        MsvcRoot         = $msvcRoot
        MsvcBinDir       = $msvcBinDir
        ClExe            = $clExe
        LinkExe          = $linkExe
        LibExe           = $libExe
        DumpbinExe       = $dumpbinExe
        WindowsSdkRoot   = $sdkRoot
        WindowsSdkVer    = $sdkVersion
        DotnetExe        = $dotnetExe
        DotnetVersion    = $dotnetVersion
        HasDotnet8       = [bool]$hasDotnet8
        PythonExe        = $pythonExe
        PythonArgsPrefix = $pythonArgsPrefix
        PythonVersion    = $pythonVersion
        NodeExe          = if ($nodeCmd) { $nodeCmd.Source } else { $null }
        NpmCmd           = if ($npmCmd) { $npmCmd.Source } else { $null }
        VitestCmd        = if (Test-Path -LiteralPath $paths.VitestCmd -PathType Leaf) { $paths.VitestCmd } else { $null }
        ViteCmd          = if (Test-Path -LiteralPath $paths.ViteCmd -PathType Leaf) { $paths.ViteCmd } else { $null }
        HasFrontendDeps  = $paths.HasFrontendDeps
    }
}

function Initialize-MsvcEnv {
    [CmdletBinding()]
    param(
        $Toolchain = $null
    )

    if ($null -eq $Toolchain) {
        $Toolchain = Resolve-E2EToolchain
    }

    # Collapse duplicate PATH / Path / path keys to prevent MSBuild MSB6001
    $procVars = [System.Environment]::GetEnvironmentVariables('Process')
    $existingPathValue = $null
    $pathKeysToRemove = @()
    foreach ($k in $procVars.Keys) {
        $ks = [string]$k
        if ($ks -ieq 'PATH') {
            if ($null -eq $existingPathValue) {
                $existingPathValue = [string]$procVars[$k]
            }
            if ($ks -cne 'PATH') {
                $pathKeysToRemove += $ks
            }
        }
    }
    foreach ($rk in $pathKeysToRemove) {
        [System.Environment]::SetEnvironmentVariable($rk, $null, 'Process')
    }
    if ($null -eq $existingPathValue) {
        $existingPathValue = ''
    }

    $msvcRoot = $Toolchain.MsvcRoot
    $sdkRoot = $Toolchain.WindowsSdkRoot
    $sdkVer = $Toolchain.WindowsSdkVer

    $incList = @(
        (Join-Path $msvcRoot 'include'),
        (Join-Path $sdkRoot "Include\$sdkVer\ucrt"),
        (Join-Path $sdkRoot "Include\$sdkVer\um"),
        (Join-Path $sdkRoot "Include\$sdkVer\shared"),
        (Join-Path $sdkRoot "Include\$sdkVer\winrt"),
        (Join-Path $sdkRoot "Include\$sdkVer\cppwinrt")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container }
    $env:INCLUDE = $incList -join ';'

    $libList = @(
        (Join-Path $msvcRoot 'lib\x64'),
        (Join-Path $sdkRoot "Lib\$sdkVer\ucrt\x64"),
        (Join-Path $sdkRoot "Lib\$sdkVer\um\x64")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container }
    $env:LIB = $libList -join ';'

    $sdkBinDir = Join-Path $sdkRoot "bin\$sdkVer\x64"
    $prependDirs = @($Toolchain.MsvcBinDir, $sdkBinDir) | Where-Object { Test-Path -LiteralPath $_ -PathType Container }
    $currentParts = @($existingPathValue -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $filteredParts = @($currentParts | Where-Object {
        $p = $_
        -not ($prependDirs -contains $p)
    })
    $env:PATH = (@($prependDirs) + @($filteredParts)) -join ';'

    return $Toolchain
}

function Invoke-E2ECppBuildAndRun {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string[]]$SourceFiles,
        [Parameter(Mandatory = $true)][string]$ExePath,
        [string]$BuildDir = '',
        [string[]]$IncludeDirs = @(),
        [string[]]$Defines = @('_CRT_SECURE_NO_WARNINGS', 'NOMINMAX', 'GKMOTION_OFFLINE_TEST'),
        [string[]]$Libs = @('winmm.lib', 'user32.lib', 'shell32.lib', 'ole32.lib', 'comdlg32.lib', 'windowscodecs.lib', 'bcrypt.lib', 'advapi32.lib'),
        [ValidateSet('/W3', '/W4')][string]$WarningLevel = '/W3',
        [switch]$TreatWarningsAsErrors,
        [switch]$UseSrcDefaults,
        [switch]$ImGui,
        [string[]]$ExtraCompilerFlags = @(),
        [string[]]$RunArgs = @(),
        [switch]$NoRun,
        $Toolchain = $null
    )

    $paths = Get-E2EPaths
    if ($null -eq $Toolchain) {
        $Toolchain = Resolve-E2EToolchain -RepoRoot $paths.RepoRoot
    }
    Initialize-MsvcEnv -Toolchain $Toolchain | Out-Null

    $resolvedExe = [System.IO.Path]::GetFullPath($ExePath)
    if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        $BuildDir = [System.IO.Path]::GetDirectoryName($resolvedExe)
    } else {
        $BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
    }
    if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    }
    $exeParent = [System.IO.Path]::GetDirectoryName($resolvedExe)
    if (-not (Test-Path -LiteralPath $exeParent -PathType Container)) {
        New-Item -ItemType Directory -Path $exeParent -Force | Out-Null
    }

    $allIncludes = New-Object System.Collections.Generic.List[string]
    if ($UseSrcDefaults) {
        foreach ($d in $paths.CppIncludeDirs) {
            if (-not $allIncludes.Contains($d)) { $allIncludes.Add($d) }
        }
    } elseif (-not $allIncludes.Contains($paths.CommonDir)) {
        $allIncludes.Add($paths.CommonDir)
    }
    foreach ($d in $IncludeDirs) {
        if (-not [string]::IsNullOrWhiteSpace($d)) {
            $full = [System.IO.Path]::GetFullPath($d)
            if (-not $allIncludes.Contains($full)) { $allIncludes.Add($full) }
        }
    }

    $allSources = New-Object System.Collections.Generic.List[string]
    foreach ($s in $SourceFiles) {
        $fullS = [System.IO.Path]::GetFullPath($s)
        if (-not (Test-Path -LiteralPath $fullS -PathType Leaf)) {
            throw "C++ source file not found: '$fullS'"
        }
        $allSources.Add($fullS)
    }

    $allDefines = New-Object System.Collections.Generic.List[string]
    foreach ($def in $Defines) {
        if (-not [string]::IsNullOrWhiteSpace($def) -and -not $allDefines.Contains($def)) {
            $allDefines.Add($def)
        }
    }

    if ($ImGui) {
        if (-not $allIncludes.Contains($paths.ImGuiDir)) {
            $allIncludes.Add($paths.ImGuiDir)
        }
        foreach ($imDef in @('GKMOTION_ENABLE_IMGUI', 'IMGUI_ENABLE_TEST_ENGINE')) {
            if (-not $allDefines.Contains($imDef)) { $allDefines.Add($imDef) }
        }
        foreach ($imCpp in @('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp')) {
            $imPath = Join-Path $paths.ImGuiDir $imCpp
            if (-not (Test-Path -LiteralPath $imPath -PathType Leaf)) {
                throw "ImGui source file not found: '$imPath'"
            }
            $allSources.Add($imPath)
        }
    }

    $clArgs = New-Object System.Collections.Generic.List[string]
    $clArgs.Add('/nologo')
    $clArgs.Add('/std:c++20')
    $clArgs.Add('/utf-8')
    $clArgs.Add('/EHsc')
    $clArgs.Add('/MT')
    $clArgs.Add('/O1')
    $clArgs.Add($WarningLevel)
    if ($TreatWarningsAsErrors) {
        $clArgs.Add('/WX')
    }
    foreach ($def in $allDefines) {
        $clArgs.Add("/D$def")
    }
    foreach ($flag in $ExtraCompilerFlags) {
        if (-not [string]::IsNullOrWhiteSpace($flag)) {
            $clArgs.Add($flag)
        }
    }
    foreach ($inc in $allIncludes) {
        $clArgs.Add("/I$inc")
    }
    foreach ($src in $allSources) {
        $clArgs.Add($src)
    }
    $clArgs.Add("/Fe:$resolvedExe")
    $clArgs.Add('/link')
    foreach ($lib in $Libs) {
        if (-not [string]::IsNullOrWhiteSpace($lib)) {
            $clArgs.Add($lib)
        }
    }

    $compileOutputText = ''
    $compileExit = 0
    $runExit = 0
    $runOutputText = ''

    Push-Location -LiteralPath $BuildDir
    try {
        $oldPref = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $global:LASTEXITCODE = 0
            $compileOut = & $Toolchain.ClExe @clArgs 2>&1
            $compileExit = [int]$LASTEXITCODE
            $compileOutputText = ($compileOut | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        } finally {
            $ErrorActionPreference = $oldPref
        }

        if ($compileExit -ne 0) {
            throw "C++ build failed (exit=$compileExit) for '$resolvedExe':`n$compileOutputText"
        }

        if (-not $NoRun) {
            $oldPref2 = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $global:LASTEXITCODE = 0
                $runOut = & $resolvedExe @RunArgs 2>&1
                $runExit = [int]$LASTEXITCODE
                $runOutputText = ($runOut | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
            } finally {
                $ErrorActionPreference = $oldPref2
            }
        }
    } finally {
        Pop-Location
    }

    return [pscustomobject]@{
        ExePath         = $resolvedExe
        CompileExitCode = $compileExit
        RunExitCode     = $runExit
        CompileOutput   = $compileOutputText
        RunOutput       = $runOutputText
    }
}
