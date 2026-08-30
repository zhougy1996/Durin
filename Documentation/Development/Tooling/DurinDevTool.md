# DurinDevTool Command Interface

Summary: Define the interactive shell, repository path discovery, documentation lifecycle commands, and workspace scaffolding interface.

Last reviewed: 2026-08-30

## Interactive Shell

Running `DevTool.bat` without arguments, or passing `shell`, starts the
interactive shell. Direct invocation and interactive commands use the same
parser and execution pipeline; the shell adds session-local preset selection,
readable terminal output, history, and completion.

Opening the shell loads repository configuration and registered presets but
does not initialize CMake or the compiler toolchain. The first mutating build or
test command resolves that environment; later commands and preset switches
reuse it. Read-only and artifact commands remain available when the toolchain is
unavailable.

The prompt displays the selected preset. `preset` shows it, `preset <number>`
selects a displayed entry, and `preset <full-name>` performs case-insensitive
selection for later shell commands. Direct commands use `--preset <name>`.
A bare group with a safe read-only default executes that default; groups without
one show help. `exit` and `quit` leave the shell.

Representative commands are:

```powershell
.\DevTool.bat presets
.\DevTool.bat status
.\DevTool.bat build --target all
.\DevTool.bat test CoreConcurrencyTests FTaskSchedulerTests.*
.\DevTool.bat asset
.\DevTool.bat asset resave /Game/Characters
.\DevTool.bat cook --output Saved/Cooked --target win64 --target-profile game
.\DevTool.bat path runtime
.\DevTool.bat open logs
```

If Ctrl+C is not forwarded by the terminal or wrapper, run `DevTool.bat stop`
from a second process. It stops the active DurinDevTool operation and its CMake/
Ninja child process tree for this checkout; the foreground shell cannot accept
`stop` while waiting for its own operation.

Build, run, clean, recovery, output, and ownership behavior is defined by
[Build and Run](../Build/BuildAndRun.md). Native-test selection and execution
are defined by [Native Test Execution](../Build/NativeTests.md); target
construction is defined by [Native Test Authoring](../Build/NativeTestAuthoring.md).
Asset checking, canonical resave, and storage qualification are defined by
[Build and Run](../Build/BuildAndRun.md#asset-maintenance).

## Project Cook

`cook` runs the synchronous offline project Cook through `DurinAssetTool`
without opening an application loop:

```powershell
.\DevTool.bat cook --output Saved/Cooked --target win64 --target-profile game
.\DevTool.bat cook --output Saved/Cooked --root /Game/Levels/Entry --dry-run
.\DevTool.bat cook --output Saved/Cooked --no-incremental --json
```

`--project` selects a registered project; omission uses the configured default.
`--root` is repeatable and augments the project's configured default Level.
`--output` resolves relative to the checkout and is passed to the native host as
an absolute path. `--profile` continues to select the host build profile, while
`--target-profile` selects the Cook runtime profile. `--dry-run` performs
discovery and immutable capture without opening a store transaction.

Human output summarizes package counts, validated Cook hits, failures, changed
bytes, and reused bytes. `--json` validates and emits the version-1 Cook
run schema with packages in canonical virtual-path order. Native failure reports
are preserved even when the child exits nonzero; cancellation returns 130,
ordinary Cook failure returns 1, and success returns 0. The child process uses
the standard heartbeat, interruption, and command-log policy.

## Repository Locations

`path <location>` writes one absolute native path without a label. `path --all
--plain` writes stable tab-separated `name<TAB>path` records. Names are
case-insensitive:

| Location | Resolved directory |
| --- | --- |
| `root` | Current checkout root |
| `build` | Selected preset's CMake binary directory |
| `binaries` or `bin` | Runtime-binaries root |
| `output` | Selected platform/configuration output |
| `runtime` | Selected runtime variant |
| `tests` | Selected runtime variant's native-test binaries |
| `logs` | DurinDevTool command logs |

`path` reports registered locations before they exist. `open` requires an
existing directory and reports the command expected to create it when absent.
Neither command initializes the toolchain or acquires the checkout build lock;
only `open` launches an external process. Arbitrary filesystem paths are not
accepted.

## Documentation Commands

Discovery excludes plan and roadmap archives unless `--include-archive` is
explicit:

```powershell
.\DevTool.bat doc list --under Documentation\Runtime
.\DevTool.bat doc find "asset package" --kind contract
.\DevTool.bat doc refs Documentation\Runtime\Assets\AssetPackages.md
.\DevTool.bat doc plan context ComputeRendererIntegration
.\DevTool.bat doc validate --scope changed
```

`list`, `find`, `refs`, and `validate` support terminal, Markdown, and
schema-versioned JSON output. Validation checks mechanical repository rules;
it does not replace ownership review against `Documentation/AGENTS.md`.

Create specialized documentation files directly from the minimal template in
the nearest `AGENTS.md`, then run the applicable validator. Structural
mutations apply and validate transactionally by default; pass `--dry-run` to
preview without writing:

```powershell
.\DevTool.bat doc move Documentation\Runtime\Old.md Documentation\Runtime\New.md
.\DevTool.bat doc move Documentation\Runtime\Old.md Documentation\Runtime\New.md --dry-run
```

Each mutation verifies current fingerprints, writes atomically, repairs
Markdown references where required, validates repository documentation plus
plan and roadmap lifecycle metadata, and rolls back on failure. Its success
output is the validation receipt; do not immediately repeat an equivalent
validator. The former `--apply` spelling remains accepted for compatibility.

`doc plan context <query>` resolves exactly one plan and emits its metadata,
current status, per-stage task counts, the first stage with open tasks, the
immediately preceding handoff when present, and Related Code. Use `--scope` for
completed or archived plans and `--format json` for structured consumption.
Task, plan, and roadmap lifecycle commands follow their nearest authoring rules.
The concise Agent workflow is [Agent Documentation Workflow](../../Agents/Documentation.md).

## Create A Module

Create a module with one command. The project descriptor may be relative to the
workspace root or absolute:

```powershell
.\DevTool.bat create module Gameplay --project Sandbox\Sandbox.dproject --kind runtime --private-dependency Core --private-dependency Engine
.\DevTool.bat create module SceneEditor --project Sandbox\Sandbox.dproject --kind editor --private-dependency DurinEd
.\DevTool.bat create module AssetRecipes --project Sandbox\Sandbox.dproject --kind developer --private-dependency Core
```

Without `--path`, runtime modules use `Source/Runtime/<Name>` and `BaseModules`;
editor modules use `Source/Editor/<Name>` and the DurinEditor extra-module set;
developer modules use `Source/Developer/<Name>` and the same default host set.
`--kind` selects scaffolding and default enablement, not a permanent build-system
classification. `--path` may select another nonoverlapping location inside the
owning project.

Use `--enable none`, `--enable base`, or repeated `--enable <RuntimeVariant>` to
replace default enablement. Public, private, optional-public, and
optional-private dependency options are repeatable. Shared linkage and a self
PCH are default; `--link static` and `--pch <Name>` select alternatives.

Preview without creating files or changing descriptors:

```powershell
.\DevTool.bat create module Gameplay --project Sandbox\Sandbox.dproject --private-dependency Core --dry-run --plain
```

Creation validates names, dependencies, variants, paths, and CMake collisions
before writing. It tracks new paths and backs up the descriptor so failure
restores prior bytes and removes only paths created by that invocation. Module
files come from `Templates/Scaffolding/module`.

## Create A Workspace Project

Create and register a workspace-local project with:

```powershell
.\DevTool.bat create project MyGame --path MyGame
```

The path must be a new direct child of the workspace root. Names are valid C++
identifiers and case-insensitively unique among projects, modules, and CMake
targets. The command creates the project descriptor, CMake setup, `Configs` and
`Content`, a same-named runtime module, and the root CMake registration.

Use `--dry-run --plain` to preview. Project-tree creation and the root CMake edit
form one transaction: failure restores the root file byte-for-byte and removes
only paths created by the invocation. Installed-engine projects, external roots,
and nested workspace project paths are not supported. Project files come from
`Templates/Scaffolding/project`; the initial module uses the module templates.

Workspace ownership and descriptor semantics are defined by
[Workspace And Projects](../../Workspace/WorkspaceProjects.md).
