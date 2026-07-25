[CmdletBinding()]
param(
    [Alias("dry-run")]
    [switch] $DryRun
)

$ErrorActionPreference = "Stop"

function Get-Worktrees {
    param([Parameter(Mandatory)] [string] $RepositoryRoot)

    $safeRepositoryRoot = $RepositoryRoot.Replace("\", "/")
    $lines = & git.exe `
        -c "safe.directory=$safeRepositoryRoot" `
        -c "core.quotePath=false" `
        -C $RepositoryRoot `
        worktree list --porcelain
    if ($LASTEXITCODE -ne 0) {
        throw "Could not enumerate Git worktrees."
    }

    $worktrees = @()
    $currentWorktree = $null
    foreach ($line in $lines) {
        if ($line.StartsWith("worktree ")) {
            if ($null -ne $currentWorktree) {
                $worktrees += $currentWorktree
            }
            $currentWorktree = [pscustomobject]@{
                Path = $line.Substring("worktree ".Length)
                Branch = $null
            }
        }
        elseif ($null -ne $currentWorktree -and $line.StartsWith("branch refs/heads/")) {
            $currentWorktree.Branch = $line.Substring("branch refs/heads/".Length)
        }
    }
    if ($null -ne $currentWorktree) {
        $worktrees += $currentWorktree
    }

    return $worktrees
}

function Get-EnvironmentArguments {
    param([Parameter(Mandatory)] [string] $Worktree)

    $configPath = Join-Path $Worktree ".agents\build-config.json"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        Write-Warning "Agent config is missing for worktree '$Worktree'. Opening it without a configured environment; run Setup.bat there to create the config."
        return @()
    }

    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $setupScript = [string] $config.environmentSetup.script
    if ([string]::IsNullOrWhiteSpace($setupScript)) {
        return @()
    }
    if (-not (Test-Path -LiteralPath $setupScript -PathType Leaf)) {
        throw "Environment setup script does not exist: '$setupScript'."
    }

    # Each pane initializes independently because Windows Terminal may already
    # be running and therefore cannot reliably inherit this process environment.
    return @($setupScript) + @($config.environmentSetup.arguments | ForEach-Object { [string] $_ })
}

function Add-TerminalPane {
    param(
        [Parameter(Mandatory)] [System.Collections.Generic.List[string]] $Arguments,
        [Parameter(Mandatory)] [ValidateSet("new-tab", "split-pane")] [string] $Action,
        [Parameter(Mandatory)] [string] $Worktree,
        [string[]] $EnvironmentArguments = @(),
        [string] $SplitDirection
    )

    $Arguments.Add($Action)
    if ($SplitDirection) {
        $Arguments.Add($SplitDirection)
    }

    $Arguments.Add("--startingDirectory")
    $Arguments.Add($Worktree)
    $Arguments.Add("--title")
    $Arguments.Add((Split-Path -Leaf $Worktree))
    $Arguments.Add("cmd.exe")
    $Arguments.Add("/k")
    foreach ($environmentArgument in $EnvironmentArguments) {
        $Arguments.Add($environmentArgument)
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
$discoveredWorktrees = @(Get-Worktrees -RepositoryRoot $repositoryRoot)
if ($discoveredWorktrees.Count -eq 0) {
    throw "Git did not report any worktrees."
}

# Keep Git's order within each group while ensuring the primary integration
# branches receive the first and most prominent terminal panes.
$worktrees = @(
    $discoveredWorktrees | Where-Object { $_.Branch -eq "main" }
    $discoveredWorktrees | Where-Object { $_.Branch -eq "dev" }
    $discoveredWorktrees | Where-Object { $_.Branch -notin @("main", "dev") }
)

Write-Host "Durin worktrees ($($worktrees.Count)):"
$environmentArgumentsByPath = @{}
foreach ($worktree in $worktrees) {
    $branchLabel = if ($worktree.Branch) { $worktree.Branch } else { "detached" }
    Write-Host "  [$branchLabel] $($worktree.Path)"
    # Resolve every config before opening a partially initialized window.
    $environmentArgumentsByPath[$worktree.Path] = @(Get-EnvironmentArguments -Worktree $worktree.Path)
}

Write-Host "Layout: up to four panes per tab (2 x 2)."
if ($DryRun) {
    Write-Host "Dry run complete; Windows Terminal was not opened."
    exit 0
}

if (-not (Get-Command wt.exe -ErrorAction SilentlyContinue)) {
    throw "wt.exe was not found. Install Windows Terminal or enable its app execution alias."
}

$terminalArguments = [System.Collections.Generic.List[string]]::new()
$terminalArguments.Add("-w")
$terminalArguments.Add("new")

for ($index = 0; $index -lt $worktrees.Count; $index++) {
    $position = $index % 4

    if ($index -gt 0) {
        $terminalArguments.Add(";")
    }

    switch ($position) {
        0 {
            Add-TerminalPane -Arguments $terminalArguments -Action "new-tab" -Worktree $worktrees[$index].Path -EnvironmentArguments $environmentArgumentsByPath[$worktrees[$index].Path]
        }
        1 {
            Add-TerminalPane -Arguments $terminalArguments -Action "split-pane" -SplitDirection "-V" -Worktree $worktrees[$index].Path -EnvironmentArguments $environmentArgumentsByPath[$worktrees[$index].Path]
        }
        2 {
            Add-TerminalPane -Arguments $terminalArguments -Action "split-pane" -SplitDirection "-H" -Worktree $worktrees[$index].Path -EnvironmentArguments $environmentArgumentsByPath[$worktrees[$index].Path]
        }
        3 {
            # After the right side is split, focus the full-height left pane and
            # split it to complete an evenly sized 2 x 2 grid.
            $terminalArguments.Add("move-focus")
            $terminalArguments.Add("left")
            $terminalArguments.Add(";")
            Add-TerminalPane -Arguments $terminalArguments -Action "split-pane" -SplitDirection "-H" -Worktree $worktrees[$index].Path -EnvironmentArguments $environmentArgumentsByPath[$worktrees[$index].Path]
        }
    }
}

& wt.exe @terminalArguments
exit $LASTEXITCODE
