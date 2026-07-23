# BuildTool Shell Consistency Plan

Last reviewed: 2026-07-24

## Current Status

Stages 1 and 2 are complete. Direct action normalization and host-aware shell
tokenization preserve command arguments across interfaces. Interactive failures
now retain the active request and derived context, measure the actual operation
time, use action-specific or neutral titles as appropriate, and keep the session
available after recoverable validation, child-command, and interruption errors.
Canonical diagnostics omit compatibility slash prefixes while slash-prefixed
input remains accepted. The complete Agent tooling suite passes with 85 tests.

Stage 3 is next. The independently maintained shell command grammar still has
the interface-parity gaps described below.

## Goal

Make direct BuildTool commands and the interactive shell one coherent interface:
arguments retain their exact meaning on every supported host, equivalent commands
accept equivalent options and defaults, and failures preserve enough context for
the user to recover without reconstructing the operation from earlier log output.

## Scope

- Correct direct-CLI action normalization without rewriting option values.
- Parse interactive shell command lines according to the active host's quoting
  and escaping rules.
- Preserve action, preset, target, command, and elapsed-time context when a shell
  operation fails.
- Define one command specification that drives direct CLI parsing, interactive
  shell dispatch, validation, and help text.
- Align command defaults and option availability between direct and interactive
  use.
- Expose read-only discovery commands consistently where they are useful outside
  the shell.
- Defer expensive toolchain initialization until a command needs it while
  retaining one resolved environment per interactive session.
- Extend the Python tooling tests and update the operational documentation.

## Non-Goals

- Changing CMake presets, build profiles, output directories, or target naming.
- Changing checkout locking, interruption recovery, process-tree termination, or
  purge safety boundaries.
- Adding command completion, command history persistence, or a full-screen
  terminal UI.
- Replacing `argparse` for direct CLI parsing.
- Making `run` build the runtime implicitly.
- Redesigning native-test discovery or GoogleTest filtering.

## Design Decisions and Invariants

- Direct CLI and shell commands share one command specification. A command's
  canonical name, aliases, positional operands, options, defaults, validation,
  and help summary are not duplicated across dispatch paths.
- Lowercase remains canonical and command names remain case-insensitive. Option
  values and application arguments are byte-for-byte unaffected by command-name
  normalization.
- On Windows, shell tokenization follows Windows command-line quoting semantics;
  on other hosts it follows the platform's existing POSIX shell semantics.
  Unquoted Windows paths such as `C:\Temp\foo` must retain every backslash.
- `build` and `rebuild` default to target `all` in both interfaces. `test`
  continues to require an explicit target. This matches the existing shell
  workflow and removes the current asymmetry where the more expensive `rebuild`
  has a default but direct `build` does not.
- The shell accepts the same named options as the direct CLI. Existing compact
  shell forms such as `build Core` and `test CoreTests FJsonDocumentTests.*`
  remain compatibility aliases and normalize into the canonical request model.
- An option is advertised only for commands that use it. Session-wide options
  supplied when opening the shell remain available as defaults for subsequent
  commands.
- `presets`, `status`, and `open-runtime` are available as direct read-only
  commands as well as shell commands. Their output remains script-friendly under
  `--plain`.
- Entering the shell resolves repository configuration, build profiles, and
  presets immediately, but resolves CMake, the toolchain environment, and
  required commands only before the first command that needs them. The resolved
  tool environment is then reused for the rest of that shell session.
- Locking remains operation-scoped. An idle interactive shell does not own the
  checkout build lock.
- Purge confirmation, registered-root constraints, recovery markers, and stop
  behavior remain unchanged.

## Current Foundations and Gaps

- `argparse` already provides command-specific direct CLI parsers, typed numeric
  bounds, and structured `CommandRequest` values.
- `BuildContext` and `derive_context` already support session-local preset
  selection without changing machine-local configuration.
- Build execution, recovery tracking, purge safety, and runtime process ownership
  are centralized below the CLI layer and do not need redesign.
- Direct action normalization is subcommand-aware and leaves known global-option
  values unchanged.
- Interactive tokenization now has explicit Windows and POSIX paths; Windows
  drive paths and backslashes reach runtime arguments unchanged.
- Shell command parsing and help are handwritten separately from the direct CLI,
  which has produced different target defaults, unavailable test timeout control,
  and options that are accepted but unused.
- Shell failure reporting retains request/context details, child command and
  exit information, recovery guidance, and the measured operation duration.
- `BuildTool shell --help` describes only shell startup options, not the commands
  available after entering the shell.
- Tooling tests cover host-specific tokenization, command-like option values,
  Windows shell dispatch, and shell parse, validation, child-command, and
  interruption failures. Cross-interface parity remains uncovered until Stage 3.

## Implementation Stages

### Stage 1: P0 Argument Integrity

- [x] Replace whole-argument scanning in direct action normalization with
  subcommand-aware normalization that cannot mutate or interpret option values.
- [x] Introduce a host-aware interactive command tokenizer with explicit Windows
  and POSIX behavior.
- [x] Preserve quoted spaces, empty arguments, backslashes, option-like runtime
  arguments, and GoogleTest filter punctuation.
- [x] Convert malformed quoting into a concise BuildTool usage error without
  terminating the interactive session.
- [x] Add focused tests for mixed-case commands following command-like option
  values and for representative Windows and POSIX argument strings.

#### Acceptance Gate

- Direct commands remain case-insensitive regardless of preceding option values.
- Windows drive paths and backslashes reach `CommandRequest.run_arguments`
  unchanged.
- Quoted arguments and malformed-input behavior are deterministic on every
  supported host.
- All existing Agent tooling tests and the new argument-integrity tests pass.

### Stage 2: P1 Failure Context and Recovery Feedback

- [x] Track the active child request, derived context, and start time around every
  shell-dispatched operation.
- [x] Report the actual action, preset, target, command, exit code, and elapsed
  time when those values are available.
- [x] Use an action-neutral error title when failure happens before a build
  action is known, and an action-specific title after dispatch.
- [x] Keep recoverable command errors inside the shell while preserving the
  existing process-interruption and recovery-marker semantics.
- [x] Remove compatibility slash prefixes from canonical error wording while
  continuing to accept leading slashes as input aliases.
- [x] Add tests for parse failures, validation failures, child-command failures,
  interrupted operations, and continued shell use after each recoverable error.

#### Acceptance Gate

- A failed long-running shell operation never reports `0.00s` unless it actually
  completed in less than the display precision.
- Failure output identifies the same operation context in direct and interactive
  use.
- Recovery guidance and marker behavior remain unchanged for interrupted build
  state.

### Stage 3: P1 Shared Command Model and Interface Parity

- [ ] Define shared metadata for command names, aliases, supported options,
  positional operands, defaults, and help summaries.
- [ ] Generate or configure both direct parsers and shell dispatch from that
  metadata while keeping execution in the existing core layer.
- [ ] Make `build` and `rebuild` default to `all` in both interfaces and keep
  `test` target-required.
- [ ] Support canonical named options in the shell, including test filter and
  timeout, while retaining documented compact forms as compatibility aliases.
- [ ] Stop accepting command options that have no effect; keep only the common
  options actually consumed by each direct command.
- [ ] Add direct `presets`, `status`, and `open-runtime` actions with stable
  `--plain` output.
- [ ] Make top-level help, subcommand help, `shell --help`, and in-shell `help`
  derive from the shared command descriptions.
- [ ] Add table-driven parity tests covering defaults, options, invalid operands,
  aliases, and help for every command.

#### Acceptance Gate

- Equivalent direct and shell invocations produce equivalent `CommandRequest`
  values.
- No command advertises or accepts an option that it ignores.
- All available commands are discoverable without first entering an interactive
  session.
- Compatibility shell forms continue to work and canonical help presents one
  preferred syntax.

### Stage 4: P2 Shell Startup and Interaction Polish

- [ ] Split lightweight repository/profile/preset context creation from
  toolchain-backed context preparation.
- [ ] Resolve and cache the toolchain environment on the first configure, build,
  clean, rebuild, or test operation that requires it.
- [ ] Allow presets, status, purge, run, and open-runtime to operate when the
  compiler toolchain is unavailable, subject to their existing command-specific
  requirements.
- [ ] Make status distinguish unresolved session defaults from a resolved
  toolchain context without implying that a validation has already occurred.
- [ ] Use a distinct preset-selection prompt and make cancellation and invalid
  numeric selection explicit.
- [ ] Keep an idle shell lock-free and verify that a second BuildTool process can
  still run `stop` or acquire the operation lock.
- [ ] Update BuildTool shell documentation and examples after behavior is
  validated.

#### Acceptance Gate

- Opening a shell for read-only or artifact-management commands does not require
  CMake, Visual Studio environment discovery, or compiler validation.
- The first toolchain-backed command performs normal validation exactly once per
  shell session, and later preset switches reuse the resolved environment.
- Status output clearly indicates whether toolchain values are resolved.
- Lock ownership and cross-process stop behavior match the pre-change contract.

## Validation Matrix

| Area | Required Evidence |
| --- | --- |
| Direct parsing | Mixed-case commands before and after every common option position; command-like option values remain unchanged |
| Windows shell parsing | Drive paths, UNC paths, quoted paths, trailing backslashes, empty arguments, and application flags |
| POSIX shell parsing | Quoted spaces, escaped characters, empty arguments, and application flags |
| Interface parity | Table-driven direct/shell request comparison for every command and compatibility alias |
| Failure reporting | Parse, validation, child exit, timeout, and interruption cases with real context and elapsed time |
| Help | Top-level, per-command, shell startup, in-shell, styled, and `--plain` output |
| Lazy initialization | Read-only commands without a toolchain; first build initialization; reuse after preset selection |
| Ownership safety | Existing lock, stop, recovery-marker, purge-boundary, and Windows process-job tests |
| Documentation | Operational examples match accepted syntax, defaults, and recovery behavior |

Use the repository setup and validation workflow documented in
`Documentation/Setup/BuildAndRun.md`. The Python Agent tooling suite is the
primary automated gate; a full native engine build is required only if the
implementation changes execution below the CLI/context boundary.

## Definition of Done

- Every implementation-stage acceptance gate passes.
- The Agent tooling suite covers the identified regressions and passes in full.
- Direct and interactive help expose one canonical command model.
- Windows paths and option values are never altered by parsing or normalization.
- Shell failures retain accurate operation context and elapsed time.
- Accepted options all have observable semantics.
- BuildTool operational documentation reflects the landed interface.
- Lasting command and context contracts are documented outside this plan before
  archival.

## Deferred Follow-ups

- Interactive command completion and persistent history.
- Shell aliases beyond the existing slash and compact positional compatibility
  forms.
- Machine-readable JSON status output.
- Runtime target discovery and completion.
- A richer terminal UI or background-operation model.

## Related Documentation

- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Architecture/BuildSystem.md`
- `Documentation/Setup/NativeTests.md`
- `Documentation/Issues/BuildToolWindowsLockRecovery.md`

## Related Code

- `BuildTool.bat`
- `Engine/Scripts/Build/durin_build_tool/cli.py`
- `Engine/Scripts/Build/durin_build_tool/config.py`
- `Engine/Scripts/Build/durin_build_tool/core.py`
- `Engine/Scripts/Build/durin_build_tool/output.py`
- `Engine/Scripts/Tests/test_agent_tooling.py`
