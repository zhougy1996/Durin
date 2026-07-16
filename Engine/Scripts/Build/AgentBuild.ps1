[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("Shell", "Configure", "Build", "Clean", "Rebuild", "Test", "Purge", "Run")]
    [string]$Action = "Build",

    [Parameter(Position = 1)]
    [string]$Target,

    [ValidateRange(0, 256)]
    [int]$Jobs = 0,

    [string]$Filter,

    [string[]]$RunArguments,

    [switch]$AllPresets,

    [switch]$Yes,

    [string]$Profile,

    [Alias("CMakePath")]
    [string]$CMakeCommand,

    [Alias("VsDevCmdPath")]
    [string]$EnvironmentSetupScript
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$PythonScript = Join-Path $PSScriptRoot "durin_build_tool.py"
$VenvPython = Join-Path $RepoRoot ".venv\Scripts\python.exe"

if (Test-Path -LiteralPath $VenvPython -PathType Leaf) {
    $Python = $VenvPython
}
else {
    throw "Durin's Python environment was not found. Run Setup.bat first."
}

$Arguments = @($PythonScript, $Action)
if ($Jobs -gt 0) {
    $Arguments += @("--jobs", "$Jobs")
}
if ($Target) {
    $Arguments += @("--target", $Target)
}
if ($Filter) {
    $Arguments += @("--filter", $Filter)
}
if ($AllPresets) {
    $Arguments += "--all-presets"
}
if ($Yes) {
    $Arguments += "--yes"
}
if ($Profile) {
    $Arguments += @("--profile", $Profile)
}
if ($CMakeCommand) {
    $Arguments += @("--cmake", $CMakeCommand)
}
if ($EnvironmentSetupScript) {
    $Arguments += @("--environment-setup", $EnvironmentSetupScript)
}
if ($RunArguments) {
    $Arguments += "--args"
    $Arguments += $RunArguments
}

& $Python @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "Durin BuildTool failed with exit code $LASTEXITCODE."
}
