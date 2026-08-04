# DurinDevTool Command Interface Refinement Plan

Summary: Refine DurinDevTool around explicit location resolution, predictable shell semantics, precise command options, and centralized compatibility handling.

Last reviewed: 2026-08-04

Status: Completed
Completed: 2026-08-04

## Current Status

DurinDevTool already drives direct CLI parsing and interactive-shell dispatch
from one command registry, keeps preset selection session-local, and defers
toolchain initialization until an operation needs it. The remaining interface
issues are concentrated around commands whose behavior is more specialized or
stateful than their names imply:

- `open-runtime` combines selected-preset path resolution with launching a
  platform file manager and exposes only one artifact location.
- The interactive `presets` command enters a modal selection prompt even though
  the direct command only lists presets.
- Bare `worktree` defaults to opening terminals for every registered worktree.
- Shared argument bundles expose child-output options on commands that do not
  launch a child whose output can use them.
- `help` lists top-level commands but cannot resolve a command or nested command
  path such as `help build` or `help worktree add`.

All stages are complete. Baseline characterization fixes the selected location
vocabulary and deprecation wording and explicitly captures the standalone
`open-runtime`, shared-output-option, and top-level-only help behavior that later
stages replace. Repository searches found no tracked automation outside the
tooling tests and owning build documentation that depends on the `Preset>` mode,
bare `worktree` opening terminals, or `open-runtime` spelling. The implementation
baseline is commit `d5a13620`; Stage 0 landed in `0f00a004`. A typed location
registry now owns the seven selected locations and `bin` alias, runtime and test
executable paths consume it, status reuses its build-directory resolver, and a
separate host opener owns external file-manager dispatch. The Stage 1 location
and build-core matrix passes 71 tests. Canonical `path` and `open` commands now
share typed requests and location resolution, raw path output remains directly
capturable, irrelevant child-output options are rejected, and `open-runtime`
normalizes through centrally declared deprecation metadata without a standalone
action. The Stage 2 focused registry, location, and build-core matrix passes 94
tests; direct `help`, `path root`, `path bin`, and plain `path --all` smoke tests
also pass. The shell no longer has a `Preset>` mode, numeric and named preset
selectors share one resolver in direct and interactive use, bare `worktree`
lists safely, and nested help is generated from leaf command metadata. The Stage
3 registry, worktree, and shell matrix passes 89 tests; direct nested-help,
numeric-preset, and bare-worktree smoke tests also pass. The lasting command and
location contracts now live in the build-and-run and build-system documentation.
The complete DurinDevTool suite passes 256 tests; the test process explicitly
prepended the bundled Ninja directory because this machine's Visual Studio lives
under the non-standard `D:\Programs` root. A scripted interactive shell verified
list-only presets, numeric selection, retained selection, runtime path output,
and nested help. All-plan validation passes. Stage commits are `0f00a004`,
`27ae4ae4`, `5ad05372`, and `0d187cac`; this final stage contains documentation,
completion metadata, and aggregate validation only.

## Goal

Make DurinDevTool's direct CLI and interactive shell predictable from command
names alone: path inspection is separate from external-process side effects,
listing does not implicitly enter a mode, bare command groups do not perform a
surprising action, every accepted option changes observable behavior, and help
can describe any registered command path.

## Scope

- Add a typed, repository-owned location model for frequently used build and
  artifact directories.
- Add canonical `path <location>` and `open <location>` commands shared by the
  direct CLI and interactive shell.
- Replace the standalone `open-runtime` implementation with a centrally managed
  deprecated alias for `open runtime`.
- Reuse the location model from runtime launch, native-test path construction,
  status output, and related diagnostics where doing so removes duplicate path
  assembly.
- Make `presets` list only and make `preset <name-or-number>` the explicit
  preset-selection operation.
- Change bare `worktree` behavior from opening terminals to listing registered
  worktrees.
- Split command argument bundles so context, styling, toolchain, and child-output
  options are registered only where their semantics are observable.
- Add nested command help without duplicating parser or command metadata.
- Update tests and the owning build-and-run documentation for the resulting
  interface.

## Non-Goals

- Replacing `argparse`, the shared `CommandRegistry`, or the line-oriented
  interactive shell.
- Moving frequent lifecycle commands such as `build`, `test`, `run`, and
  `status` under a new top-level command group.
- Accepting arbitrary filesystem paths in `open`; it remains a selector for
  registered repository locations rather than a general file-manager wrapper.
- Adding an interactive `cd` command, because DurinDevTool cannot change the
  parent process's working directory.
- Adding command completion, persistent history, a full-screen terminal UI, or
  background command execution.
- Changing CMake output layout, preset names, build profiles, checkout locking,
  recovery markers, or runtime process ownership.
- Adding JSON output to commands that do not already have a machine-readable
  format.
- Performing a native engine refactor or changing runtime behavior.

## Design Decisions and Invariants

### Command vocabulary

- `path <location>` is the canonical non-mutating location command. For one
  location it writes exactly one absolute native path to stdout, without a
  label, so callers can capture it directly.
- `path --all` displays every available canonical location and its resolved
  path. Plain output uses one stable tab-separated record per location; styled
  terminal output may use a table.
- `open <location>` resolves the same location, verifies that it currently
  exists as a directory, and opens it with the host file manager.
- `dir` is not introduced as a canonical command because it conventionally
  means listing directory contents on Windows. The intended compact workflows
  are `path bin` and `open bin`.
- `open-runtime` becomes a deprecated command alias for `open runtime`. It is
  omitted from canonical help, emits one concise migration warning per process,
  and does not retain a separate action or execution path. Physical removal of
  the alias is deferred until a later compatibility review.

### Location contract

The initial registered locations are fixed and case-insensitive:

| Canonical location | Meaning | Context requirement |
| --- | --- | --- |
| `root` | Current repository checkout root | Repository |
| `build` | Selected CMake preset's `binaryDir` | Profile and preset |
| `binaries` | Configured runtime-binaries root, currently `Engine/Binaries` | Repository config |
| `output` | Selected platform and configuration directory below `binaries` | Profile and preset |
| `runtime` | Selected runtime variant directory containing its launcher | Profile and preset |
| `tests` | Selected runtime variant's native-test `Bin` directory | Profile and preset |
| `logs` | DurinDevTool command-log directory below the configured state directory | Repository config |

- `bin` is a documented location alias for `binaries`; aliases resolve to the
  canonical location before execution and `path --all` lists only canonical
  names.
- Every repository-relative source path comes from tracked repository config,
  CMake preset metadata, or an existing typed resolver. The commands do not
  embed a second copy of the output layout.
- `path` resolves a valid location even when its directory has not been created.
  Missing output is useful diagnostic information and is not a command failure.
- `open` requires an existing directory. Its error identifies the resolved path
  and gives location-specific recovery guidance when a build or command run is
  expected to create it.
- Static and preset-derived location inspection does not initialize CMake, the
  compiler toolchain, parallelism, or the checkout operation lock.
- Only `open` launches an external process. `path`, `presets`, `preset`,
  `status`, and `worktree list` remain free of GUI side effects.

### Registry and compatibility ownership

- Direct CLI parsing and interactive-shell parsing continue to share one
  `CommandSpec` tree.
- Canonical command names, ordinary aliases, deprecated aliases, migration
  text, and help visibility are declared in command metadata rather than in
  shell-specific normalization code.
- A deprecated alias normalizes before handler dispatch. Handlers receive only
  canonical action data and cannot branch on how the caller spelled a command.
- Alias matching remains case-insensitive and retains the existing leading-slash
  compatibility behavior.
- The location registry owns location names, aliases, context requirements,
  resolution, display ordering, and missing-directory recovery text.
- The host file-manager launcher is a separate service that accepts a resolved
  existing directory; location resolvers never launch processes.

### Shell semantics

- `presets` always lists presets and returns to the normal
  `DurinDevTool>` prompt. It never consumes the next input line.
- `preset` without an operand displays the current preset.
- `preset <full-name>` selects that preset using the existing case-insensitive
  full-name rule. `preset <number>` selects the corresponding number from the
  stable registered-preset ordering shown by `presets`.
- Direct and interactive `preset <selector>` resolve the selector identically.
  Only an interactive process retains the selection for later commands; a
  direct invocation reports the resolved selection and exits.
- Bare `worktree` uses the safe `list` default. Opening terminals requires the
  explicit `worktree open` spelling.
- Bare command groups without a deliberately selected safe default display
  group help rather than guessing an action.

### Arguments, help, and failures

- Repository/profile/preset context options, styling options, toolchain options,
  child-output options, and command-specific options are separate metadata
  bundles.
- `--output` is accepted only when a command launches or controls child output
  for which `auto`, `compact`, `progress`, or `full` changes behavior.
- Existing options used by `status` to inspect unresolved toolchain defaults
  remain accepted because they affect reported values.
- `help [COMMAND ...]` resolves a canonical command, alias, or nested command
  path and renders help from the same parser metadata used for execution.
- `COMMAND --help` and `help COMMAND` describe the same accepted operands,
  options, defaults, and nested commands.
- Unknown location and command-path failures list bounded valid choices and do
  not terminate an interactive shell.

## Current Foundations and Gaps

- `CommandRegistry` already owns top-level and nested command metadata, parser
  creation, feature filtering, handler loading, and direct/shell dispatch. It
  lacks explicit alias, deprecation, and nested-help metadata.
- `runtime_executable_path` already derives the selected runtime launcher, and
  `preset_build_directory` already expands and constrains CMake `binaryDir`.
  Related output and test paths are still assembled in separate functions.
- `open_runtime_directory` contains a usable cross-platform opening strategy,
  but combines path selection, existence policy, host dispatch, error wording,
  and success output in one runtime-specific function.
- Repository configuration already owns the runtime-binaries and state
  directories, so the new location model can remain layout-driven.
- `run_shell` adds preset-selection modal state after registry dispatch. That
  state creates the main direct/shell semantic difference and can be removed
  once numeric selectors are supported by `preset` itself.
- `CONTEXT_ARGUMENTS` currently includes `OUTPUT_MODE`, which makes it too broad
  for discovery and external-opening commands.
- Worktree commands already use a nested registry and have a safe `list`
  implementation; only the selected default needs to change.
- Existing registry and build-core tests cover direct/shell request parity,
  preset selection, command help, platform opening, and runtime paths. They need
  characterization updates before behavior is changed.

## Implementation Stages

### Stage 0: Freeze the command and compatibility contract

- [x] Add characterization tests for current direct and shell parsing of
  `open-runtime`, `presets`, `preset`, bare `worktree`, nested help, and
  context/output options.
- [x] Add table-driven contract cases for every planned canonical location,
  alias, context requirement, missing-directory policy, and display order.
- [x] Record the final command examples and exact deprecation warning expected
  for `open-runtime` in focused tests before implementation.
- [x] Confirm repository searches find no automation that requires the modal
  `Preset>` prompt or relies on bare `worktree` opening terminals.
- [x] If a real in-repository dependency conflicts with the selected contract,
  record the dependency and decision change in this plan before continuing.

#### Acceptance Gate

- The intended command grammar and location meanings are represented by failing
  or characterization tests without changing production behavior.
- Every intentional compatibility break or retained alias has a named caller,
  migration path, and test expectation.
- No unresolved command-name, location-name, or default-action decision remains.

### Stage 1: Introduce typed location and opener services

- [x] Add a typed location registry whose entries declare canonical name,
  aliases, display order, required context, resolver, and missing-directory
  recovery guidance.
- [x] Represent a resolved location with its canonical name, absolute path, and
  existence state so formatting and opening do not repeat filesystem queries.
- [x] Extract the Windows, macOS, and Linux file-manager launch behavior into a
  platform opener that accepts only an existing directory.
- [x] Implement resolvers for `root`, `build`, `binaries`, `output`, `runtime`,
  `tests`, and `logs` using repository config and selected preset/profile data.
- [x] Refactor runtime executable and native-test path helpers to consume shared
  location results while preserving their current filenames and validation.
- [x] Reuse the shared `build`, `runtime`, and log location resolvers in status
  or diagnostics where they currently duplicate the same path assembly.
- [x] Keep context creation lazy: location resolution must not prepare the
  compiler environment, resolve build jobs, or acquire the checkout lock.

#### Acceptance Gate

- Unit tests cover every location on all supported host/profile shapes,
  including paths containing spaces and directories that do not yet exist.
- Runtime launch and native-test executable paths are byte-for-byte unchanged
  for every registered preset in the test fixture matrix.
- Platform-opener tests mock external process APIs and verify that no opener is
  invoked for a missing or non-directory path.
- Location-only requests leave toolchain preparation and checkout locking
  untouched.

### Stage 2: Add canonical path and open commands

- [x] Register `path` and `open` in the shared command model with identical
  direct and interactive grammar.
- [x] Implement one-location and `path --all` formatting, including stable raw
  and tab-separated plain output.
- [x] Make `open` use the same resolved location object as `path` and report a
  concise success message containing the canonical location and absolute path.
- [x] Extend command metadata with canonical aliases and deprecated aliases,
  including help visibility and migration warnings.
- [x] Normalize `open-runtime` to `open runtime`, remove the standalone
  `Action.OPEN_RUNTIME` execution branch, and retain only the centralized
  deprecated alias registration.
- [x] Split `CONTEXT_ARGUMENTS` into precise context, display, toolchain, and
  child-output bundles; reject previously accepted options that had no
  observable effect.
- [x] Ensure feature filtering and prepared-environment checks are derived from
  the selected canonical command and location requirements rather than from the
  deprecated spelling.

#### Acceptance Gate

- `path LOCATION`, `open LOCATION`, `path bin`, and `open-runtime` parse and
  dispatch equivalently through direct and interactive entry paths where their
  semantics overlap.
- `path` output is directly capturable as one absolute path and succeeds before
  a selected artifact directory exists.
- `open` opens each existing registered directory and produces actionable,
  location-specific failures for missing directories.
- Canonical help advertises `path` and `open`, omits `open-runtime`, and the
  deprecated alias emits its warning exactly once per process.
- Discovery and opening commands reject child-output and toolchain options they
  do not consume.

### Stage 3: Remove implicit shell modes and unsafe defaults

- [x] Remove `selecting_preset`, the `Preset>` prompt, and the post-`presets`
  dispatch hook from the interactive shell.
- [x] Move numeric preset resolution into the canonical `preset` request path
  while retaining full-name selection and session-local persistence.
- [x] Make direct `preset <number>` report the same resolved preset that an
  interactive shell would retain.
- [x] Change the `worktree` default subcommand from `open` to `list`; preserve
  explicit `worktree open` behavior and `--dry-run` support.
- [x] Implement `help [COMMAND ...]` by resolving the existing registry tree,
  including nested commands and aliases.
- [x] Make a bare group with no safe default render its group help inside the
  interactive shell without exiting the shell.
- [x] Remove obsolete compact-normalization and prompt tests, replacing them
  with direct/shell parity and continued-session tests for the new semantics.

#### Acceptance Gate

- After `presets`, the next input is parsed as a normal command.
- `preset 2` and `preset <full-name>` select the same preset in direct and shell
  parsing, and an interactive selection is reused by later commands.
- Bare `worktree` performs no GUI launch and produces the same registered list
  as `worktree list`.
- `help build`, `help worktree add`, and their `--help` equivalents expose the
  same grammar without duplicating command descriptions.
- Parse and command errors remain recoverable inside the interactive shell.

### Stage 4: Migrate documentation and validate the complete interface

- [x] Replace canonical `open-runtime` examples with `open runtime` and add
  representative `path build`, `path bin`, `path runtime`, and `open logs`
  workflows to the owning build-and-run guide.
- [x] Document the location table, missing-directory behavior, safe worktree
  default, explicit preset selection, nested help, and deprecated alias policy.
- [x] Search tracked scripts, templates, tests, and documentation for old
  command spellings or assumptions and migrate every canonical caller.
- [x] Run the complete DurinDevTool Python test suite using the repository
  workflow in the build-and-run guide.
- [x] Run focused direct-command and scripted interactive-shell smoke tests for
  paths, opening with mocked or dry-run boundaries, preset selection, worktree
  listing, option rejection, and nested help.
- [x] Run all-plan documentation validation and update this plan's status and
  evidence-backed checklists.

#### Acceptance Gate

- The owning guide presents only canonical commands while documenting the one
  retained deprecated alias.
- Repository-owned automation and tests no longer invoke `open-runtime` or rely
  on the removed `Preset>` interaction or bare-worktree side effect.
- The complete DurinDevTool Python suite and documentation validation pass.
- No native engine build is required when the implementation remains above the
  CMake execution and runtime layers; if implementation changes those layers,
  validation expands according to the root build-and-run contract.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Registry grammar | Table-driven canonical command, alias, deprecated alias, nested-command, and invalid-option parsing |
| Direct/shell parity | Equivalent requests for every new command and preset selector through both entry paths |
| Location resolution | Exact absolute paths for all locations across preset, platform, configuration, and runtime-variant fixtures |
| Missing paths | `path` succeeds with a resolved path; `open` fails before launching and reports targeted recovery guidance |
| Host opening | Mocked `os.startfile`, `open`, and `xdg-open` invocation with spaces and native path syntax |
| Lazy context | No toolchain, job, environment, or checkout-lock preparation for path/open discovery operations |
| Runtime compatibility | Existing runtime and native-test executable paths remain unchanged |
| Preset interaction | List-only `presets`, explicit numeric/name selection, session reuse, cancellation-free normal prompt |
| Worktree safety | Bare `worktree` equals `worktree list`; only explicit `worktree open` starts terminals |
| Help | `help`, nested `help COMMAND ...`, `COMMAND --help`, aliases, plain output, and in-shell recovery |
| Deprecation | Hidden canonical help entry, one warning per process, canonical dispatch, and no duplicate action branch |
| Documentation | Build-and-run examples, tracked-reference search, changed-scope validation, and all-plan validation |

## Definition of Done

- Every implementation-stage acceptance gate passes.
- Direct and interactive users can resolve or open every registered location
  without a runtime-specific command implementation.
- `open-runtime` is only a centrally declared deprecated alias for
  `open runtime`; no runtime-specific opener action remains.
- `presets` has no modal follow-up state, and explicit preset selection accepts
  both stable numbers and full names.
- Bare `worktree` is side-effect free.
- Every registered option has tested observable semantics, and irrelevant
  options are rejected with command help guidance.
- Nested help is generated from the same command model used for execution.
- Runtime, test, build, and log paths have one typed resolution owner each.
- Lasting behavior is documented in the build-and-run guide rather than relying
  on this plan as a competing interface specification.
- The complete DurinDevTool Python suite and all-plan validator pass.
- The plan records completion evidence, `Status: Completed`, and a completion
  date before entering the normal archive workflow.

## Deferred Follow-ups

- Physical removal of the deprecated `open-runtime` alias after a later
  compatibility review.
- Shell completion and persistent command history.
- Machine-readable JSON output for status and multi-location inspection.
- Runtime target discovery and completion.
- Opening individual files or arbitrary user-supplied paths.
- A general artifact browser or full-screen terminal UI.

## Related Documentation

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Plans/Archive/2026-07/BuildToolShellConsistency.md`
- `Documentation/Plans/Archive/2026-07/DurinDeveloperToolConsolidation.md`

## Related Code

- `Tools/DurinDevTool/durin_dev_tool/registry.py`
- `Tools/DurinDevTool/durin_dev_tool/shell.py`
- `Tools/DurinDevTool/durin_dev_tool/commands/core.py`
- `Tools/DurinDevTool/durin_dev_tool/configuration.py`
- `Tools/DurinDevTool/durin_dev_tool/build/config.py`
- `Tools/DurinDevTool/durin_dev_tool/build/operations.py`
- `Tools/DurinDevTool/durin_dev_tool/build/runtime.py`
- `Tools/DurinDevTool/durin_dev_tool/build/process.py`
- `Tools/DurinDevTool/durin_dev_tool/worktree/handler.py`
- `Tools/DurinDevTool/tests/test_build_registry.py`
- `Tools/DurinDevTool/tests/test_build_core.py`
- `Tools/DurinDevTool/tests/test_build_config.py`
- `Tools/DurinDevTool/tests/test_bootstrap_worktree.py`
