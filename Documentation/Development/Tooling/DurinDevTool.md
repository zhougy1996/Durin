# DurinDevTool Command Interface

Summary: Define command output, the interactive shell, repository path discovery, documentation lifecycle commands, and workspace scaffolding interface.

Last reviewed: 2026-10-17

## Command Discovery

`help` groups commands by purpose and includes common examples. Run
`help <command>` or `<command> --help` for the full interface. Command help
separates routine options from advanced toolchain and display overrides.
Bare command groups without a safe default, such as `create` and `dependency`,
show the same help in direct invocations and the shell. `help <command-path>`
supports Tab completion in the shell.

`build [TARGET]` and `rebuild [TARGET]` take one positional CMake target,
defaulting to `all`. Pass application arguments after `run --`; options before
the separator select DevTool context, and everything after it reaches the
application unchanged.

Options may appear between multiple positional selections, for example
`asset resave /Game/A --dry-run /Game/B`. Values belonging to an option still
follow that option. `run` retains its explicit `--` boundary: application
arguments after it are forwarded unchanged and are not parsed as DevTool options.

Dependency preparation follows the same positional selection style:
`dependency prepare tracy tracy-tools --config debug`, or `dependency prepare
--all`. Configuration values are lowercase (`debug`, `release`, `all`); see
[Third-Party Dependency Preparation](../Build/ThirdPartyBootstrap.md#entry-points).

## Preview And Execution

Action commands execute by default. Commands with preview support accept
`--dry-run` to skip their primary change.
This rule is identical in direct invocations and the interactive shell.

| Commands | Default action | With `--dry-run` |
| --- | --- | --- |
| `asset resave`, `asset material-template`, `asset material-functions` | Save selected packages, create a template, or initialize missing DefaultMaterial | Inspect and report without saving authored assets |
| `create module`, `create project` | Create and register the requested item | Validate and show the creation plan |
| `doc move`, `doc task remove`, `doc plan archive`, `doc roadmap archive` | Apply and validate the transactional change | Show the change without applying it |
| `worktree prepare`, `worktree remove` | Prepare links or remove the selected worktree | Show the operation without modifying the worktree |
| `worktree open` | Launch worktree terminals | List planned terminal launches |
| `cook` | Publish a Cook generation | Prepare and report without publishing a generation |

Preview is a per-command capability, not a global switch. Commands without that
capability reject `--dry-run`; test selection uses its read-only `--explain`
interface. Read-only discovery commands need neither flag, and bare `asset`
still selects read-only `check`. `--yes` answers purge confirmation and `--force`
permits discarding worktree changes; neither selects preview or execution.

Preview skips the primary change, not all incidental writes. Native asset and
Cook processes can write diagnostic logs and runtime/cache state; Cook writes
its DDC and logs under the selected output directory even during a dry run.
See the owning command contract for the exact boundary.

## Command output and logs

Output options have separate meanings:

| Option | Purpose and supported commands |
| --- | --- |
| `--log-mode auto\|compact\|progress\|full` | Child-process log presentation for configure, build, clean, recover, rebuild, test, and run |
| `--output-dir PATH` | Cook output directory; independent of stdout format |
| `--format terminal\|json` | Cook and asset check/resave result format |
| `--format terminal\|markdown\|json` | Documentation results, limited to each command's supported formats |
| `--color auto\|always\|never` | Styled build, setup/dependency, scaffolding, location, and plan/roadmap listing output |

Long options must be spelled in full. `test --report [PATH]` selects the XML
report file; it does not select stdout format or log presentation. Commands that
only emit one format, such as `asset identity-audit`, keep that fixed format.

`--color auto` is the default: it enables ANSI styling for terminals unless
`NO_COLOR` is set or `TERM=dumb`. Explicit `always` and `never` override automatic
detection and `NO_COLOR`. Forcing color does not enable progress output on a
redirected stream; log presentation still uses actual terminal detection.
JSON and Markdown result renderers never add ANSI styling.

Output separates resolved context, execution stages, child-process output, and
results. Pass `--color never` to disable ANSI styling:

```powershell
.\DevTool.bat build <Target> --color never
```

Child-process output has four modes, selected with
`--log-mode auto|compact|progress|full`. The default `auto` mode selects progress
output in an interactive terminal and compact output when stdout is redirected
or consumed by an Agent. Progress mode updates routine Ninja `[n/total]` status
in place and hides DHT DEBUG/INFO lines from the terminal while preserving DHT
warnings, other child output, compiler diagnostics, and the complete command
log. Empty child-output records do not finalize an active Ninja progress line.
Use `--log-mode full` when routine DHT diagnostics must also be streamed.
When explicitly requested without an interactive terminal, progress mode falls
back to compact output.
Compact mode keeps stage boundaries, command lines, heartbeats, and final
results, but suppresses routine CMake, Ninja, and successful GoogleTest lines.
It writes the complete raw output under `Build/.agent-state/logs/`, reports the
log path before each child command starts, and prints a bounded diagnostic
excerpt plus the log path when a child fails. The newest 40 command logs are
retained. Use `--log-mode full` to stream every child-output line, or
`--log-mode compact` to suppress routine child output in an interactive terminal:

```powershell
.\DevTool.bat build <Target> --log-mode progress
.\DevTool.bat build <Target> --log-mode compact
.\DevTool.bat test CoreConcurrencyTests --log-mode full
```

Compact native-test runs also enable GoogleTest's brief output mode. Test
failures and the final test summary remain in the captured output and failure
excerpt; application or library messages are always preserved in the full log.
In styled terminal output, GoogleTest and CTest running, passed, skipped, and
failed statuses are colored consistently even when the child process disables
its own terminal colors.
`--color` controls styling independently and does not select an output volume.

Compact mode emits a short heartbeat every 30 seconds while a child command
remains alive. It reports elapsed time and the latest CMake/Ninja status when
available, including Ninja's completed count and action. Logs are flushed as
lines arrive so they can be read during execution. Non-interactive callers need
no extra flags. Use `--color never`
and `--log-mode compact` to request the same display explicitly in a terminal.
Progress and full output modes stream child output without default heartbeats.

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

The prompt displays the selected preset. `presets` lists short names, full CMake
names, and numbered entries. Prefer stable short names such as `release-editor`
or `debug-game`; names are case-insensitive. Both `preset <selector>` and direct
`--preset <selector>` accept the same short name, full name, or displayed number.
Short names are derived from configuration and runtime variant; if two presets
share a short name, use the full name. Inside the shell, `preset <selector>`
selects the preset for later commands. Direct `preset <selector>` inspects that
selection for the current invocation; direct operations use `--preset`.
`preset` alone inspects the current selection.

Tab completes command and `help` paths, option names (including `--help`), and
option choices. Preset values complete short names, full names, and displayed
numbers after `preset`, `--preset`, or `--preset=`; candidates follow the shell's
current profile or an explicit `--profile`. Dependency lists such as
`--deps Core En` complete registered workspace modules at every list position;
the next option ends the list. Completion uses current repository metadata
without initializing the build toolchain. Application arguments after `run --`
are left to the application.

A bare group with a safe read-only default executes that default; groups without
one show help. `exit` and `quit` leave the shell.
`doc`, `doc task`, `doc plan`, `doc roadmap`, and `worktree` default to `list`
in both direct invocations and the shell; `asset` defaults to read-only `check`.

Representative commands are alternatives, not a sequence:

```powershell
.\DevTool.bat presets
.\DevTool.bat status
.\DevTool.bat build <Target>
.\DevTool.bat test CoreConcurrencyTests FTaskSchedulerTests.*
.\DevTool.bat test CoreConcurrencyTests FTaskSchedulerTests.* --explain
.\DevTool.bat asset
.\DevTool.bat asset resave /Game/Characters
.\DevTool.bat asset resave /Game/Characters --dry-run
.\DevTool.bat cook --output-dir Saved/Cooked
./DevTool cook --output-dir Saved/Cooked
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
All test selections use `--explain` for read-only resolution; `test list [query]`
discovers targets. Explanation preserves the execution selection's mode and
filter rules.
Asset checking and current-format canonical resave are defined by
[Build and Run](../Build/BuildAndRun.md#asset-maintenance).

## Project Cook

`cook` runs the synchronous offline project Cook through `DurinAssetTool`
without opening an application loop:

```powershell
.\DevTool.bat cook --output-dir Saved/Cooked
.\DevTool.bat cook --output-dir Saved/Cooked --root /Game/Levels/Entry --dry-run
.\DevTool.bat cook --output-dir Saved/Cooked --no-incremental --format json
```

`--project` selects a registered project; omission uses the configured default.
`--root` is repeatable and augments the project's configured default Level.
`--output-dir` resolves relative to the checkout and is passed to the native host as
an absolute path. `--profile` continues to select the host build profile, while
`--target` optionally selects `win64` or `macos`; omission derives it from the
selected host build profile's platform. An unsupported platform requires an
explicit target. Cook uses the `game` runtime profile. `--dry-run` performs
discovery and preparation without opening a store transaction.

Cook reports use JSON schema version 2 and `packages[].serializer` to identify
the package writer; the retired contributor field/report version has no fallback.
Reports distinguish `captured`, `cook-hit` and `omitted` package outcomes.
Editor-only or target-excluded roots are omitted successfully; an all-omitted run
publishes an empty package manifest/state. Mixed result lists are sorted by
package path, and the human summary counts omissions separately from failures.

Each invocation starts one dedicated Cook process and exits when it finishes.
Save intended edits and finish imports before launch. Keep participating packages,
companions, external files, shader sources, configuration and native build code
stable until the process exits. Unsaved editor objects are excluded. Editing,
synchronizing or checking out inputs during Cook is unsupported; discard those
outputs and rerun from stable inputs. Cook does not prevent or diagnose such edits.

Output must be outside mounted content, shader, source and configuration trees;
existing filesystem aliases are resolved before accepting it. Cook writes DDC
and shader intermediates under `<output>/DerivedDataCache` and logs under
`<output>/Logs`, including dry runs. Reports use stdout. Cook never resaves or
repairs authored packages. These writable destinations must remain outside inputs.

Human output summarizes package counts, validated Cook hits, failures, changed
bytes, and reused bytes. `--format json` validates and emits the version-2 Cook
run schema with packages in canonical virtual-path order. Native failure reports
are preserved even when the child exits nonzero; cancellation returns 130,
ordinary Cook failure returns 1, and success returns 0. The child process uses
the standard heartbeat, interruption, and command-log policy.

## Repository Locations

`path <location>` writes one absolute native path without a label. `path --all
--color never` writes stable tab-separated `name<TAB>path` records. Names are
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
validator. Omit `--dry-run` to execute the change.

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
.\DevTool.bat create module Gameplay --project Sandbox\Sandbox.dproject --deps Core Engine
.\DevTool.bat create module SceneEditor --project Sandbox\Sandbox.dproject --kind editor --deps DurinEd
.\DevTool.bat create module AssetRecipes --project Sandbox\Sandbox.dproject --kind developer --deps Core
```

Without `--path`, runtime modules use `Source/Runtime/<Name>` and `BaseModules`;
editor modules use `Source/Editor/<Name>` and the DurinEditor extra-module set;
developer modules use `Source/Developer/<Name>` and the same default host set.
`--kind` selects scaffolding and default enablement, not a permanent build-system
classification. `--path` may select another nonoverlapping location inside the
owning project.

Use `--enable none`, `--enable base`, or repeated `--enable <RuntimeVariant>` to
replace default enablement. `--deps Core Engine` selects private dependencies;
use `--public-deps`, `--optional-deps`, or `--optional-public-deps` for the other
visibility/optionality combinations. Each accepts multiple names and may be
repeated. Put the module name before these lists; the next option ends a list.
Shared linkage and a self PCH are default; `--link static` and `--pch <Name>`
select alternatives.

Preview without creating files or changing descriptors:

```powershell
.\DevTool.bat create module Gameplay --project Sandbox\Sandbox.dproject --deps Core --dry-run --color never
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
`Content`, a same-named runtime module, and the `Durin.dworkspace` project entry.

Use `--dry-run --color never` to preview. Project-tree creation and the workspace manifest edit
form one transaction: failure restores the manifest byte-for-byte and removes
only paths created by the invocation. Installed-engine projects, external roots,
and nested workspace project paths are not supported. Project files come from
`Templates/Scaffolding/project`; the initial module uses the module templates.

Workspace ownership and descriptor semantics are defined by
[Workspace And Projects](../../Workspace/WorkspaceProjects.md).
