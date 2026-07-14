[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("Configure", "Build", "Test")]
    [string]$Action = "Build",

    [Parameter(Position = 1)]
    [string]$Target,

    [ValidateRange(1, 256)]
    [int]$Jobs = 14,

    [string]$Filter,

    [string]$CMakePath,

    [string]$VsDevCmdPath
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$LocalAgentFile = Join-Path $RepoRoot "AGENTS_LOCAL.md"
$AgentPreset = "Win64-Debug-DurinEditor-Agent"
$AgentBuildDir = Join-Path $RepoRoot "Build\$AgentPreset"
$AgentTestBinDir = Join-Path $RepoRoot "Engine\Binaries\Win64\Debug-Agent\Tests\DurinEditor\Bin"

function Get-LocalAgentSetting {
    param([string]$Name)

    if (-not (Test-Path -LiteralPath $LocalAgentFile)) {
        return $null
    }

    $content = Get-Content -Raw -LiteralPath $LocalAgentFile
    $pattern = "(?m)^- $([regex]::Escape($Name)):\s+``([^``]+)``\s*$"
    $match = [regex]::Match($content, $pattern)
    if (-not $match.Success -or $match.Groups[1].Value.StartsWith("<")) {
        return $null
    }

    return $match.Groups[1].Value
}

function Resolve-CMakePath {
    if ($CMakePath) {
        return $CMakePath
    }
    if ($env:DURIN_CMAKE_PATH) {
        return $env:DURIN_CMAKE_PATH
    }

    $localPath = Get-LocalAgentSetting "Preferred CMake"
    if ($localPath) {
        return $localPath
    }

    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "CMake was not found. Set 'Preferred CMake' in AGENTS_LOCAL.md, DURIN_CMAKE_PATH, or pass -CMakePath."
}

function Resolve-VsDevCmdPath {
    if ($VsDevCmdPath) {
        return $VsDevCmdPath
    }
    if ($env:DURIN_VSDEVCMD_PATH) {
        return $env:DURIN_VSDEVCMD_PATH
    }

    $localPath = Get-LocalAgentSetting "Visual Studio env script"
    if ($localPath) {
        return $localPath
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installationPath) {
            $detectedPath = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
            if (Test-Path -LiteralPath $detectedPath) {
                return $detectedPath
            }
        }
    }

    throw "VsDevCmd.bat was not found. Set 'Visual Studio env script' in AGENTS_LOCAL.md, DURIN_VSDEVCMD_PATH, or pass -VsDevCmdPath."
}

function Import-VisualStudioEnvironment {
    param([string]$Path)

    $environmentLines = & $env:ComSpec /d /s /c "call `"$Path`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize the Visual Studio developer environment using '$Path'."
    }

    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf("=")
        if ($separator -le 0) {
            continue
        }

        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -Path "Env:$name" -Value $value
    }
}

function Invoke-CMake {
    param([string[]]$Arguments)

    & $script:ResolvedCMakePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake failed with exit code $LASTEXITCODE."
    }
}

function Ensure-AgentConfigured {
    $cacheFile = Join-Path $AgentBuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cacheFile)) {
        Invoke-CMake @("--preset", $AgentPreset)
    }
}

function Assert-Target {
    if (-not $Target) {
        throw "-$Action requires -Target <target-name>."
    }
    if ($Target -notmatch "^[A-Za-z0-9_.+-]+$") {
        throw "Target contains unsupported characters: '$Target'."
    }
}

$ResolvedCMakePath = Resolve-CMakePath
$ResolvedVsDevCmdPath = Resolve-VsDevCmdPath

if (-not (Test-Path -LiteralPath $ResolvedCMakePath)) {
    throw "CMake does not exist at '$ResolvedCMakePath'."
}
if (-not (Test-Path -LiteralPath $ResolvedVsDevCmdPath)) {
    throw "VsDevCmd.bat does not exist at '$ResolvedVsDevCmdPath'."
}

Import-VisualStudioEnvironment $ResolvedVsDevCmdPath
$PreviousLocation = Get-Location
try {
    Set-Location $RepoRoot

    switch ($Action) {
        "Configure" {
            Invoke-CMake @("--preset", $AgentPreset)
        }
        "Build" {
            Assert-Target
            Ensure-AgentConfigured
            Invoke-CMake @("--build", $AgentBuildDir, "--target", $Target, "-j", "$Jobs")
        }
        "Test" {
            Assert-Target
            Ensure-AgentConfigured
            Invoke-CMake @("--build", $AgentBuildDir, "--target", $Target, "-j", "$Jobs")

            $testExecutable = Join-Path $AgentTestBinDir "$Target.exe"
            if (-not (Test-Path -LiteralPath $testExecutable)) {
                throw "Test target '$Target' did not produce '$testExecutable'."
            }

            $testArguments = @()
            if ($Filter) {
                $testArguments += "--gtest_filter=$Filter"
            }

            & $testExecutable @testArguments
            if ($LASTEXITCODE -ne 0) {
                throw "Test target '$Target' failed with exit code $LASTEXITCODE."
            }
        }
    }
}
finally {
    Set-Location $PreviousLocation
}
