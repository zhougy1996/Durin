# Tracy Profiler Tool Integration Plan

Summary: Install the matching Tracy Windows tools through setup and expose safe multi-instance profiling workflows from DurinEditor.

Last reviewed: 2026-07-27

## Current Status

Stages 0-2 completed on 2026-07-27. Stage 3 is next. Stage 2 started from
commit `eda4de6063731c7dbb97d4830539786e8bbb9313`; the preceding runtime-variant
and initial Tracy instrumentation plan remains established at commit
`efccf9e4734947dcf56b6c1cca26dbdba9432c6d`.

Durin already builds one shared Tracy v0.13.1 client for each Release Profiling
runtime process. Matching upstream Windows capture tools were validated
manually, but they currently live under the disposable `Build/Tools/` tree and
are not restored by `Setup.bat`. DurinEditor has no repository-owned action for
launching the profiler, explaining a missing installation, opening captures, or
identifying several profiling processes.

The selected first integration keeps the official Tracy profiler in a separate
process launched and managed as a development tool by DurinEditor. Embedding
Tracy's private `tracy::View` UI directly into the Editor is deferred because
Tracy v0.13.1 builds a patched ImGui v1.92.5-docking stack while Durin owns
MonaImGui v1.92.8, and the profiler UI is not a stable embedding API.

## Goal

Make the matching Tracy v0.13.1 Windows profiler and capture executables
available after normal repository setup, then let a developer launch the
profiler and reach profiling captures directly from DurinEditor.

The workflow must distinguish simultaneous DurinEditor and DurinGame processes
without requiring ImGui or other profiling UI inside the Game runtime. Multiple
profiling processes must retain Tracy's automatic port selection and remain
discoverable by process identity rather than relying on one hard-coded
`127.0.0.1:8086` target.

## Scope

- Add a pinned, development-only Windows tool package for the matching Tracy
  v0.13.1 release.
- Prepare that package through root setup, focused Tracy setup, and worktree
  preparation using the existing third-party bootstrap ownership model.
- Give profiling Editor and Game processes stable, human-readable Tracy program
  identities containing the runtime variant, project, and process id.
- Add DurinEditor development-tool actions for launching the official profiler,
  opening the capture directory, opening a selected capture, and diagnosing a
  missing or mismatched installation.
- Preserve simultaneous profiling processes and verify Editor/Editor and
  Editor/Game discovery and capture.
- Document GUI and command-line capture workflows, installation paths, version
  matching, multi-instance selection, and failure recovery.

## Non-Goals

- Rendering Tracy's complete profiler UI inside a DurinEditor ImGui panel.
- Copying or maintaining Tracy profiler source as repository-owned UI code.
- Adding ImGui, an overlay, a console, or profiling controls to DurinGame.
- Automatically building a profiling preset from inside DurinEditor.
- Replacing Tracy's network protocol, discovery UI, trace format, or upstream
  capture executables.
- Supporting remote capture, non-localhost connections, GPU zones, allocation
  tracking, sampling, or performance budgets.
- Enabling Tracy or installing its runtime DLL in ordinary Debug, Release, or
  Shipping outputs.
- Assigning a permanent fixed port to each project or checkout.

## Design Decisions and Invariants

### Tool Package Ownership

- Tracy client source and Tracy host tools are separate development artifacts.
  The existing `tracy` direct-source dependency continues to own
  `Engine/External/Source/tracy` for `TracyClient.dll`.
- A distinct `tracy-tools` development-only package owns the matching upstream
  host executables. It must not be represented as a linkable engine dependency.
- The canonical installed location is
  `Engine/External/Packages/tracy-tools/<Version>/<Platform>/`. It is
  machine-prepared and ignored like other external packages, not committed
  binaries and not a final runtime-output directory.
- The initial supported package is upstream Tracy v0.13.1
  `windows-0.13.1.zip` for Win64. The manifest pins the release URL, records an
  expected SHA-256 digest, and validates at least `tracy-profiler.exe`,
  `tracy-capture.exe`, and `tracy-csvexport.exe`.
- Root `Setup.bat` prepares both Tracy client source and matching host tools as
  development dependencies. `Setup_tracy.bat` repairs both artifacts together.
- Unsupported hosts skip the Windows-only tool package with an explicit status;
  they do not make preparation of the cross-platform Tracy client source fail.
- A missing host tool never blocks configure, build, launch, or instrumentation.
  It disables only Editor tool actions that require the executable.

### External Profiler Boundary

- DurinEditor launches the official profiler as a separate process. It does not
  load Tracy server/profiler implementation objects into the Editor process.
- The external process boundary prevents profiler crashes, fonts, file dialogs,
  renderer backends, global ImGui state, and analysis overhead from becoming
  DurinEditor runtime ownership.
- Durin locates tools through one repository-owned development-tool path helper;
  Editor modules do not reconstruct package paths independently.
- Launch failures include the resolved executable path, operating-system error,
  expected Tracy version, and focused repair command.
- Profiler lifetime is independent after a successful launch. Closing
  DurinEditor does not forcibly terminate an interactive profiler containing an
  unsaved capture.
- DurinEditor may pass a selected `.tracy` file to the profiler, but it does not
  parse or rewrite the Tracy trace format.

### Multi-Instance Identity And Ports

- Durin does not set `TRACY_PORT` by default. In Tracy v0.13.1, an unset port
  makes the client search up to 20 data ports beginning at 8086 and advertise
  the selected port through discovery.
- An explicit `TRACY_PORT` remains a developer override. Durin does not silently
  replace it; diagnostics must identify an explicit collision because setting a
  fixed value disables Tracy's automatic data-port search.
- Automatic Editor workflows use Tracy discovery and the advertised data port.
  They do not assume that every target listens on 8086.
- Each profiling process publishes a stable program label with runtime variant,
  project display name, and process id. The process id remains the final
  discriminator when two instances open the same project.
- The engine profiling adapter owns program-name publication. Repository call
  sites do not include Tracy headers or invoke `TracySetProgramName` directly.
- Program-name storage remains valid until Tracy has consumed it. Disabled
  builds neither evaluate profiling-only identity expressions nor acquire Tracy
  symbols.
- Discovery and direct connection remain localhost-only under the existing
  profiling security contract.

### Editor Workflow

- Profiling actions live under one `Tools > Profiling` menu owned by an
  Editor-only module or service rather than DurinGame or general runtime UI.
- The first workflow exposes:
  - launch Tracy Profiler;
  - open a chosen `.tracy` capture in the matching profiler;
  - open the repository capture directory;
  - show installation/version status and the focused repair command.
- The Editor presents the distinction between profiling the current Editor and
  profiling a separate Game process. The Game needs only the existing
  `TracyClient.dll`; all analysis UI remains in the Editor-launched external
  profiler.
- A normal Editor may launch the tool to inspect a profiling Game. Capturing the
  current Editor requires the Editor itself to come from the Release Profiling
  preset.
- Capture storage defaults beneath an ignored, workspace-local
  `Build/Profiling/Tracy/` directory. User-selected external locations remain
  valid.
- The UI never claims a connection merely because the profiler process
  launched. Connection state and target selection remain owned by Tracy unless
  a later stage adds a tested discovery/session API.

## Current Foundations and Gaps

Existing foundations:

- `Engine/Scripts/Bootstrap/thirdparty/tracy.json` pins Tracy v0.13.1 client
  source as a development-only direct-source dependency.
- Root setup already opts into development dependencies, and
  `Setup_tracy.bat` provides a focused preparation entrypoint.
- The bootstrap supports platform-specific archive packages and validates
  required installed files.
- Release Profiling presets build isolated Editor and Game outputs with one
  shared `TracyClient.dll` per process.
- Tracy is configured with `TRACY_ON_DEMAND` and `TRACY_ONLY_LOCALHOST`.
- Tracy v0.13.1 automatically searches data ports 8086 through 8105 when
  `TRACY_PORT` is unset and advertises the chosen port and process id.
- `FPlatformProcess` exposes executable-path, process-id, wait, and process
  launch facilities on Windows.
- MainFrame owns the Editor's top-level menu bar and existing development-facing
  actions.

Current gaps:

- Setup prepares client source but not the matching Windows profiler/capture
  executables.
- Archive bootstrap metadata has no recorded integrity contract for this tool
  asset.
- The validated Windows tools currently exist only in a disposable local
  `Build/Tools/` directory.
- The engine profiling adapter has zones, frame marks, and thread names but no
  program-identity operation.
- Simultaneous processes have similar executable names in Tracy discovery and
  no Durin project identity.
- DurinEditor cannot locate or launch the profiler, open captures, or explain
  how to repair a missing tool installation.
- Current profiling documentation describes upstream tool usage but not the
  managed installation, Editor workflow, or automatic multi-instance ports.

## Implementation Stages

### Stage 0: Lock Package, UX, And Validation Contracts

- [x] Revalidate the upstream v0.13.1 Windows asset name, extracted executable
  layout, BSD-3-Clause distribution obligations, and SHA-256 digest.
- [x] Inventory bootstrap schema/tests, worktree preparation, external-package
  ignore rules, Editor menu ownership, process-launch APIs, profiling startup,
  and project-name availability.
- [x] Decide whether archive digests are a general optional bootstrap field or
  narrowly owned by the new host-tool package, then record the failure
  diagnostic and test contract.
- [x] Define the exact installed path and the source-of-truth helper used by
  setup, tests, documentation, and Editor runtime lookup.
- [x] Define program-label formatting and lifetime, including missing project
  metadata and two instances of the same project.
- [x] Capture baseline tests for development-only selection, unsupported-host
  behavior, disabled profiling macros, and Editor menu registration.

#### Acceptance Gate

- Package provenance, platform behavior, UI ownership, identity format, and
  success/failure validation are explicit, and focused tests protect the
  existing setup and profiling behavior before implementation.

#### Stage Handoff

Baseline and package contract:

- Implementation-start baseline:
  `4bbd6a0b5a82d7e51e3ccdd47a33e20d8a82db21`.
- Upstream release: Tracy `v0.13.1`, asset `windows-0.13.1.zip`, published as a
  flat archive containing `tracy-profiler.exe`, `tracy-capture.exe`,
  `tracy-csvexport.exe`, `tracy-import-chrome.exe`,
  `tracy-import-fuchsia.exe`, and `tracy-update.exe`.
- Expected SHA-256:
  `ee6db1a7e71a12deb5973a8dbfdf9f36d3635bec0e0b31b1cc74f28de7dac4c9`.
- Tracy is BSD-3-Clause. The upstream binary archive does not contain the
  license file, so repository documentation accompanying the managed download
  must retain the copyright and license reference; the separately prepared
  Tracy source also retains upstream `LICENSE`.

Selected setup and path contracts:

- Add `sha256` as an optional, generally available field on each platform entry
  of an archive source. Manifest validation accepts exactly 64 hexadecimal
  digits. Acquisition computes the digest after download and before archive
  extraction; a mismatch reports the package name, archive path, expected
  digest, and actual digest and publishes no package directory.
- Add an explicit manifest-level host-platform policy for optional tools.
  `tracy-tools` skips with a status message when its archive has no entry for
  the detected host. Explicit selection and root development selection use the
  same policy. A required archive package continues to fail on a missing
  platform entry.
- The `tracy-tools` manifest is the source of truth for version, archive URL,
  digest, required executables, and `source_dir`. Its canonical Win64
  `source_dir` is
  `Engine/External/Packages/tracy-tools/0.13.1/Win64`. Bootstrap status,
  tests, documentation, and the Editor profiling service consume or validate
  that manifest instead of reconstructing a `Build/Tools` path.
- `tracy` remains order 80 and `tracy-tools` follows it. Root setup selects both
  as development dependencies. Focused `Setup_tracy.bat` explicitly selects
  both names. Worktrees continue sharing the complete `Engine/External`
  directory, so the canonical package is prepared once per dependency
  checkout rather than once per build output.
- The read-only status query returns expected version, platform support,
  resolved package and executable paths, missing required files, and the repair
  command without downloading, extracting, or creating directories.

Selected runtime and Editor contracts:

- Program labels use
  `<RuntimeVariant> | <ProjectName-or-No Project> | PID <ProcessId>`, for
  example `DurinEditor | Sandbox | PID 1234`. PID is always the final
  discriminator.
- The Core profiling adapter formats and owns stable program-name storage and
  is the only code that calls Tracy. Publication occurs in
  `FEngineLoop::PreInit` after `InitializeCurrentProject`; the Editor republishes
  after an in-process project selection. The disabled operation is a macro that
  does not evaluate its arguments.
- `TRACY_PORT` remains untouched. Unset values retain Tracy's automatic
  8086-8105 search; explicit values remain developer-owned and diagnostics
  identify the override rather than reallocating it.
- MainFrame owns the top-level menu bar. A new Editor-only profiling service
  will own tool lookup, actions, dialogs, and diagnostics; MainFrame will only
  render its `Tools > Profiling` contribution. `FPlatformProcess::LaunchProcess`
  is the existing process boundary and may be extended later only to improve
  actionable OS errors and argument handling.

Working set and key symbols inspected:

- `Engine/Scripts/Bootstrap/setup_third_party.py`:
  `validate_manifests`, `ensure_archive_source`, `process_manifest`,
  `resolve_selected_manifests`.
- `Engine/Scripts/Bootstrap/thirdparty/tracy.json`,
  `Engine/Scripts/Tests/test_agent_tooling.py`,
  `Engine/Scripts/Utils/worktree_tool.py`.
- `Engine/Source/Runtime/Core/Public/Profiling/Profiling.h`,
  `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`,
  `Engine/Tests/Native/CoreTests/Private/ProfilingTests.cpp`.
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`,
  `FPlatformProcess::LaunchProcess`, `InitializeCurrentProject`,
  `GetCurrentProject`.

Baseline validation:

- `ThirdPartyBootstrapTests`: 4 passed. These cover default exclusion,
  development/test inclusion, explicit selection, and the boolean manifest
  contract. Stage 1 adds optional-host, digest, extraction, and idempotence
  cases before changing acquisition.
- `setup_third_party.py --validate-manifests`: 9 manifests validated.
- `BuildTool test --target CoreTests --filter FProfilingTests.* --output full`:
  1 passed; disabled profiling arguments are not evaluated.
- MainFrame inspection confirms the current registered top-level menus are
  `File`, `Edit`, `Window`, and `Help`, with no `Tools` menu or profiling
  service. Stage 3 adds a focused menu-registration seam and test with the new
  contribution.

Open questions: none for Stage 1. Live Tracy discovery wording and the exact
Editor dialog presentation remain bounded Stage 3 implementation choices.

### Stage 1: Add Managed Tracy Host-Tool Preparation

- [x] Add the pinned `tracy-tools` development-only package with Win64 archive
  metadata and required executable validation.
- [x] Add archive integrity verification and focused malformed, mismatch,
  successful extraction, and already-prepared tests.
- [x] Make default root setup and focused Tracy setup prepare client source and
  matching host tools without changing ordinary build dependency graphs.
- [x] Preserve cross-platform setup by explicitly skipping an unavailable host
  package while continuing to prepare Tracy client source.
- [x] Ensure main-checkout and worktree preparation resolve one canonical
  machine-prepared package rather than downloading a copy per final-output
  directory.
- [x] Add a read-only bootstrap/status query usable by Editor diagnostics
  without triggering downloads or mutation.
- [x] Remove no files from a developer's former `Build/Tools` location; document
  it as an obsolete manual installation after the managed package is present.

#### Acceptance Gate

- A clean Win64 setup installs and verifies the matching Tracy executables at
  the canonical package path; repair is idempotent; digest corruption fails
  clearly; unsupported hosts and ordinary builds remain unaffected.

#### Stage Handoff

Baseline and working set:

- Stage baseline: `dd7a71c4b7682063836402b05b0e5da982eaab6b`.
- Added `Engine/Scripts/Bootstrap/thirdparty/tracy-tools.json`.
- Updated `Engine/Scripts/Bootstrap/setup_third_party.py`,
  `Engine/Scripts/Bootstrap/Setup_tracy.bat`,
  `Engine/Scripts/Tests/test_agent_tooling.py`, and
  `Documentation/Development/Build/ThirdPartyBootstrap.md`.
- Updated this plan with the Stage 1 result. No files beneath the former
  ignored `Build/Tools/Tracy-0.13.1` location were changed or removed.

Manifest and platform contracts:

- `tool_package` is a non-linkable bootstrap kind. `tracy-tools` is
  development-only, follows `tracy` in selection order, and pins upstream
  `windows-0.13.1.zip` with the Stage 0 SHA-256.
- Archive platform entries accept an optional `sha256` containing exactly 64
  hexadecimal digits. `compute_sha256` validates the downloaded temporary
  archive before extraction; mismatch diagnostics include expected and actual
  values and leave the package directory unpublished.
- `allow_unsupported_platform` is an explicit manifest boolean.
  `process_manifest` prints a skip status for `tracy-tools` when the detected
  host has no archive entry, while required archive packages retain their
  existing failure behavior.
- The installed Win64 path is
  `Engine/External/Packages/tracy-tools/0.13.1/Win64`. It is covered by the
  existing `Engine/External` ignore and worktree-sharing contracts and contains
  the six flat upstream executables after preparation.
- `query_manifest_status` and `--status` provide mutation-free JSON containing
  version, platform support, resolved source directory, required/missing files,
  prepared state, and repair command.
- Root setup already selects every development dependency. Focused
  `Setup_tracy.bat` now selects `tracy,tracy-tools`, preparing or repairing the
  client source and tools together without entering a CMake build graph.

Validation:

- Full `Engine.Scripts.Tests.test_agent_tooling`: 171 passed.
- Focused `ThirdPartyBootstrapTests`: 10 passed, covering development
  selection, manifest types, malformed digest, digest mismatch, successful
  extraction, already-prepared idempotence, unsupported-host skip, and
  read-only missing-file status.
- `setup_third_party.py --validate-manifests`: 10 manifests validated.
- Real `Setup_tracy.bat`: downloaded, digest-verified, and installed all six
  upstream executables; a second run reported both Tracy artifacts already
  prepared.
- `setup_third_party.py --libs tracy-tools --status`: Win64 supported,
  `prepared: true`, no missing required files, canonical path and repair command
  correct.
- `setup_third_party.py --all --with-development --status`: both `tracy` and
  `tracy-tools` selected and prepared; the query performed no mutation.

Open questions: none for Stage 2. The former ignored `Build/Tools` copy remains
untouched and is now obsolete; final end-user migration wording remains part of
Stage 4 documentation.

### Stage 2: Publish Stable Profiling Process Identity

- [x] Extend the engine-owned profiling adapter with a program-identity
  operation whose disabled form skips argument evaluation.
- [x] Publish the runtime variant, project display name, and current process id
  after project selection is authoritative and early enough for useful Tracy
  discovery.
- [x] Keep identity storage alive for Tracy's asynchronous consumption and
  update it safely if the Editor switches or relaunches a project.
- [x] Preserve Tracy's unset-port automatic search and explicit `TRACY_PORT`
  override behavior.
- [x] Add focused native tests for label formatting, fallback identity,
  disabled-build evaluation, and storage lifetime at the Durin adapter boundary.
- [x] Verify two same-project profiling Editor processes advertise distinct
  process ids and connect on distinct automatically selected ports.

#### Acceptance Gate

- Tracy discovery distinguishes concurrent Durin processes by runtime variant,
  project, and PID; automatic ports work without Durin allocation logic; and
  disabled builds remain independent of Tracy.

#### Stage Handoff

Baseline and working set:

- Stage baseline: `eda4de6063731c7dbb97d4830539786e8bbb9313`.
- Added
  `Engine/Source/Runtime/Core/Private/Profiling/Profiling.cpp`.
- Updated `Engine/Source/Runtime/Core/Public/Profiling/Profiling.h`,
  `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`,
  `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`,
  `Engine/Tests/Native/CoreTests/Private/ProfilingTests.cpp`, and
  `Documentation/Development/Build/Profiling.md`.
- Updated this plan with the Stage 2 result.

Identity and lifetime contract:

- `Profiling::FormatProgramIdentity`, `SetProgramIdentity`, and
  `GetProgramIdentity` form the Tracy-free Core adapter boundary.
  `DURIN_PROFILE_PROGRAM_IDENTITY` calls it only when `DURIN_WITH_TRACY=1`;
  the disabled macro does not evaluate runtime, project, or PID arguments.
- The exact format is
  `<RuntimeVariant> | <ProjectName-or-No Project> | PID <ProcessId>`.
  Empty runtime metadata falls back to `Durin`; empty project metadata falls
  back to `No Project`.
- `SetProgramIdentity` retains each distinct published string in a
  `std::deque<std::string>` until process shutdown. Tracy v0.13.1 temporarily
  stores the supplied `const char*` before its broadcast thread consumes it, so
  retaining prior entries prevents a project update from dangling an earlier
  pointer. Repeating an unchanged identity reuses the latest entry.
- `FEngineLoop::PreInit` publishes immediately after
  `InitializeCurrentProject`. MainFrame republishes after its in-process project
  selection succeeds. No repository call site includes Tracy internals or calls
  `TracySetProgramName` directly.
- No code reads, writes, or replaces `TRACY_PORT`. Unset ports retain upstream
  automatic search; explicit overrides remain developer-owned.

Validation:

- `BuildTool test --target CoreTests --filter FProfilingTests.* --output full`:
  4 passed for exact formatting, fallback formatting, stable storage across a
  later longer identity, and disabled macro argument evaluation.
- Full `all` builds passed for
  `Win64-Release-DurinEditor-Profiling` and
  `Win64-Release-DurinGame-Profiling`, compiling and linking the adapter through
  the real `DURIN_WITH_TRACY=1` path.
- Two final-build hidden Editors opening Sandbox advertised Tracy v3 discovery
  entries `DurinEditor | Sandbox | PID 15584` on 8086 and
  `DurinEditor | Sandbox | PID 10848` on 8087. A PID-filtered localhost
  discovery parser verified both exact strings and ports.
- Earlier in the same Stage 2 runtime validation, two concurrent Editors
  (PIDs 16796 and 13316) produced successful two-second official
  `tracy-capture.exe` traces from ports 8086 and 8087 respectively. Both traces
  contained frame and CPU-zone data.
- The official Tracy Profiler 0.13.1 discovery UI launched successfully. Its
  discovery list also contained unrelated LAN clients, so the deterministic
  acceptance evidence uses PID-filtered v3 packets rather than a scroll
  position in the GUI.

Open questions: none for Stage 3.

### Stage 3: Add DurinEditor Profiling Tool Actions

- [ ] Add one Editor-only profiling tool service that resolves the managed Tracy
  package and capture directory.
- [ ] Add `Tools > Profiling` actions for launching the profiler, opening a
  selected capture, opening the capture directory, and showing tool status.
- [ ] Extend process launching only as required to report actionable operating
  system failures and safely quote executable and capture paths.
- [ ] Disable unavailable actions with a visible explanation and
  `Setup_tracy.bat` repair command rather than silently doing nothing.
- [ ] Keep the profiler independent after launch and avoid terminating it during
  Editor shutdown.
- [ ] Add focused tests for path resolution, version/required-file validation,
  command construction, quoting, missing-tool diagnostics, and menu action
  registration.
- [ ] Document that the official profiler discovery list owns target selection
  and advertises non-8086 ports for later instances.

#### Acceptance Gate

- From DurinEditor, a developer can launch the matching official profiler, open
  a saved capture, reach the capture directory, and repair a missing
  installation without manually locating an executable.

#### Stage Handoff

- Record baseline commit, working set, Editor ownership, process-launch
  contract, UI actions, key symbols, open questions, and focused test results.

### Stage 4: Complete Multi-Instance Runtime Validation And Documentation

- [ ] Run the plan validator and all focused bootstrap, BuildTool, native, and
  Editor tests introduced or affected by the work.
- [ ] Validate ordinary Editor and Game presets remain free of Tracy runtime and
  host-tool requirements.
- [ ] Build both Release Profiling presets through BuildTool and verify their
  isolated runtime outputs.
- [ ] Launch two profiling Editors simultaneously, identify each by project and
  PID in Tracy discovery, and record successful captures from both selected
  data ports.
- [ ] Launch a profiling Editor and profiling Game simultaneously and record a
  successful Game capture without any Game-side ImGui dependency.
- [ ] Validate reconnect after stopping an on-demand capture and explicit
  `TRACY_PORT` collision diagnostics.
- [ ] Complete the repository-required full `all` build for the registered Agent
  Editor preset after the user-visible Editor actions are implemented.
- [ ] Update CPU profiling and third-party bootstrap documentation with managed
  installation, GUI/CLI usage, multi-instance behavior, repair, and capture
  storage.
- [ ] Record completion evidence, move lasting contracts to owning documents,
  close the checklist, and archive this plan.

#### Acceptance Gate

- Setup, Editor tooling, simultaneous Editor/Editor and Editor/Game capture,
  ordinary-build isolation, reconnect, documentation, focused tests, and the
  required full Editor build all pass.

#### Stage Handoff

- Record final baseline commit, complete working set, capture evidence, lasting
  documentation, validation results, deferred work, and archive location.

## Validation Matrix

| Area | Normal/disabled case | Managed/profiling case |
| --- | --- | --- |
| Setup selection | Non-development raw selection excludes Tracy artifacts | Root and focused setup prepare client source and matching host tools |
| Platform | Unsupported host continues without Windows tool package | Win64 downloads and verifies the pinned archive |
| Integrity | Existing packages keep current behavior | Corrupt or mismatched Tracy archive fails before publication |
| Build graph | Debug, Release, and Shipping do not require Tracy tools | Profiling presets use client source but do not link profiler UI |
| Tool lookup | Missing package yields a focused repair diagnostic | Canonical executable paths resolve without searching `Build/Tools` |
| Editor menu | Unavailable actions are safely disabled | Profiler, capture, and directory actions succeed |
| Identity | Disabled macros skip identity evaluation | Discovery shows runtime variant, project, and PID |
| One process | No profiler is required to run | Connect, capture, stop, reconnect, and save succeed |
| Two Editors | Ordinary Editors have no Tracy listeners | Profiling Editors select distinct automatic data ports and are distinguishable |
| Editor and Game | Game contains no profiling UI | External profiler discovers and captures Game while Editor remains open |
| Explicit port | Unset override preserves automatic search | Duplicate fixed port produces actionable diagnostics |
| Shutdown | Editor shutdown has no host-tool dependency | Interactive profiler remains independent with unsaved capture intact |
| Documentation | Ordinary build guidance remains unchanged | Setup, GUI/CLI capture, multi-instance, and repair workflows are current |

## Definition of Done

- Normal repository setup installs the matching Tracy v0.13.1 Windows host
  tools through a pinned, integrity-checked development package.
- Focused Tracy setup repairs client source and host tools together.
- Host executables are machine-prepared external packages, not committed
  binaries, runtime dependencies, or disposable build outputs.
- DurinEditor launches the official profiler and opens captures without the
  developer locating executables manually.
- Simultaneous profiling Editors and Games are distinguishable by runtime
  variant, project, and PID and connect through advertised automatic ports.
- DurinGame contains no ImGui or profiler-analysis UI dependency.
- Ordinary Debug, Release, and Shipping builds remain independent of Tracy.
- Focused tests, simultaneous live captures, plan validation, and the required
  full Editor `all` build pass.
- Lasting installation, tooling, identity, multi-instance, and recovery
  contracts live in owning documentation and the completed plan is archived.

## Deferred Follow-ups

- Embedding or adapting Tracy's private `tracy::View` UI into MonaImGui.
- A Durin-owned live discovery/session browser that bypasses Tracy's connection
  screen.
- Automated build-and-launch of a missing profiling Game artifact from Editor.
- Remote host discovery and non-localhost capture policy.
- Cross-platform packaged profiler binaries where upstream distribution and
  repository host support allow them.
- Capture history indexing, annotations, comparisons, budgets, and CI
  performance regression analysis.
- GPU, allocation, lock, sampling, call-stack, and frame-image instrumentation.

## Related Documentation

- [CPU Profiling](../Development/Build/Profiling.md)
- [Third-Party Bootstrap](../Development/Build/ThirdPartyBootstrap.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Runtime Variants](../Development/Build/RuntimeVariants.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)

## Related Code

- `Setup.bat`
- `Engine/Scripts/Bootstrap/Setup_tracy.bat`
- `Engine/Scripts/Bootstrap/setup_third_party.py`
- `Engine/Scripts/Bootstrap/thirdparty/tracy.json`
- `Engine/Scripts/Build/durin_build_tool/`
- `Engine/CMake/ThirdParty/tracy/`
- `Engine/Source/Runtime/Core/Public/Profiling/Profiling.h`
- `Engine/Source/Runtime/Core/Public/Windows/WindowsPlatformProcess.h`
- `Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformProcess.cpp`
- `Engine/Source/Editor/MainFrame/`
