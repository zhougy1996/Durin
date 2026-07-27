# Durin Developer Tool Consolidation Plan

Summary: Consolidate repository setup, worktree, build, documentation, test, run, and scaffolding workflows into one Python-based DurinDevTool under `Tools/`.

Last reviewed: 2026-07-28

Status: Archived
Completed: 2026-07-28

## Current Status

Planning was created from `main` at `23d8f1ac`, then moved to the intended
latest `dev` base after the branch mismatch was detected. The plan commit on
`dev` is `4f47cd6e`, whose parent `7f47c1bb` is the implementation baseline.
The corrected plan baseline is `8eac1950`.

The `dev` baseline already owns BuildTool under `Tools/BuildTool`, adds DocTool
under `Tools/DocTool`, and retains WorktreeTool and Setup under
`Engine/Scripts`. The plan has therefore been corrected to consolidate all
three existing Tools plus Setup rather than moving BuildTool from its older
`main` location.

Stages 0 through 6 are complete. The bootstrap-safe product skeleton now owns the
canonical launcher, validated repository discovery, standard-library-only
command registry and error boundary, shared direct/shell help, and prepared
environment capability guard. The build domain now lives under
`durin_dev_tool/build`, and the top-level registry is the sole command
specification for build, test, run, recovery, artifact, preset, and scaffolding
operations. The documentation domain now lives under
`durin_dev_tool/documentation`; `plan list`, `plan validate`, and
`plan archive` share the same typed registry and execution path in direct and
shell use while preserving their output defaults, discovery guards, validation,
and transactional archive behavior. Post-stage hardening distinguishes a missing
environment, a system-Python invocation that bypassed the launcher, and an
incomplete prepared environment before importing a dependency-backed handler.
Bootstrap-safe setup, dependency, and worktree domains now live under the same
package and registry. The atomic repository cutover is complete: current
instructions, operational documentation, active-plan references, live
investigations, CMake diagnostics, and test discovery now use DurinDevTool, and
the four old root launchers and migrated implementation trees are deleted.

Final clean-checkout and end-to-end validation is complete. The unified Python
suite and focused system-Python/bootstrap/worktree fixtures pass; direct and
scripted-shell read-only commands, dependency manifests, plan metadata, and
registered-worktree dry runs pass through DurinDevTool. The selected Agent Build
Profile completes target `all`, and the native profiling-tool service tests pass.
Compilation, stale import/reference, and diff checks found no compatibility
residue. Validation discovered no lasting behavior that was absent from the
current operational documentation.

The selected working set remains clean outside this plan's changes. The final
DurinDevTool-only Python suite passes 166 tests with one platform-dependent
skip and has no imports from the deleted implementation trees.
System-Python cold-start, launcher help, scripted shell help and
preset state, arbitrary working-directory discovery, missing-environment
failure behavior, and toolchain-free build status/preset discovery are recorded
below. The unified worktree command preserves nonzero Python failure codes,
closing the old launcher defect recorded at baseline.

The selected design
uses one canonical Windows launcher at
`Tools/DurinDevTool/DevTool.bat`, backed by the
`Tools/DurinDevTool/durin_dev_tool` Python package. Humans use the interactive
`DurinDevTool>` shell and Agents invoke the same command model directly.

Implementation will proceed through internal stages, but the repository-facing
cutover is atomic: the old root `Setup.bat`, `BuildTool.bat`, `DocTool.bat`,
and `WorktreeTool.bat` entrypoints and their old implementation locations are
deleted together. No compatibility wrappers, transition aliases, or migration
document will remain.

## Goal

Provide one discoverable development interface for a fresh checkout, prepared
main checkout, and linked worktree:

```powershell
.\Tools\DurinDevTool\DevTool
.\Tools\DurinDevTool\DevTool setup
.\Tools\DurinDevTool\DevTool build --target all --plain
.\Tools\DurinDevTool\DevTool test --target CoreTests --plain
.\Tools\DurinDevTool\DevTool plan validate --scope all
.\Tools\DurinDevTool\DevTool worktree add ..\Durin-feature -b feature-branch
```

The no-argument form opens a human-oriented shell. Argument-bearing forms are
non-interactive, return stable exit codes, and are the canonical Agent
interface. Both paths share command definitions, validation, execution, and
error semantics.

## Scope

- Add the `Tools/DurinDevTool` product directory, its single Windows launcher,
  its Python package, and colocated tests.
- Provide one top-level command registry shared by direct invocation and the
  interactive shell.
- Preserve the existing build commands and their current direct/shell parity:
  `configure`, `build`, `test`, `run`, `clean`, `rebuild`, `purge`, `presets`,
  `preset`, `status`, `open-runtime`, `stop`, and `create`.
- Preserve the existing documentation-plan listing, validation, and
  transactional archival behavior under the `plan` command family.
- Add `setup`, `worktree`, and focused third-party dependency commands to the
  same command model.
- Move repository development-tool implementation out of:
  - `Tools/BuildTool`;
  - `Tools/DocTool`;
  - `Engine/Scripts/Utils/worktree_tool.py`;
  - `Engine/Scripts/Bootstrap`.
- Preserve third-party manifests while moving them under DurinDevTool
  ownership.
- Update the editor profiling service and its native tests, which consume the
  Tracy manifest locations and manifest-provided repair command.
- Replace library-specific setup batch wrappers with typed Python commands.
- Preserve fresh-checkout setup, linked-worktree preparation, build ownership,
  interruption recovery, process-tree termination, purge boundaries, and
  worktree junction safety.
- Update current operational documentation, CMake diagnostics, repository
  instructions, active-plan references, and automated tests to the canonical
  interface.
- Remove obsolete entrypoints and implementation paths during the final
  cutover.

## Non-Goals

- Keeping compatibility wrappers or aliases for `Setup`, `BuildTool`,
  `DocTool`, or `WorktreeTool`.
- Publishing a migration guide or supporting a transition period.
- Installing DurinDevTool globally, modifying `PATH`, or requiring a shell
  profile.
- Replacing CMake presets, Agent Build Profiles, `.agents/build-config.json`,
  `requirements.txt`, or the repository `.venv` ownership model.
- Changing build output directories, runtime variants, target names,
  third-party versions, or dependency installation layouts.
- Redesigning build locking, recovery markers, Windows process jobs, purge
  safety, or worktree removal safety.
- Moving engine build-time programs such as DurinHeaderTool merely because they
  are tools.
- Rewriting archived plans to use the new product name. Archived plans remain
  historical evidence under their existing lifecycle rules.
- Adding completion, persistent history, a full-screen terminal UI, or
  background command scheduling.

## Design Decisions and Invariants

### Product and package layout

The final owned layout is:

```text
Tools/
└─ DurinDevTool/
   ├─ DevTool.bat
   ├─ durin_dev_tool/
   │  ├─ __init__.py
   │  ├─ __main__.py
   │  ├─ cli.py
   │  ├─ shell.py
   │  ├─ repository.py
   │  ├─ commands/
   │  ├─ build/
   │  ├─ documentation/
   │  ├─ worktree/
   │  └─ bootstrap/
   │     └─ thirdparty/
   └─ tests/
```

- `DevTool.bat` is the only batch entrypoint owned by the product.
- `durin_dev_tool` is the importable Python package. Product-directory casing
  does not leak into Python package names.
- `__main__.py` performs process entry only. It does not own command behavior.
- Repository-root discovery searches for stable repository markers and
  validates the result. It does not depend on a fixed `parents[n]` depth.
- Build, documentation, worktree, and bootstrap code remain separate internal
  domains rather than becoming one monolithic module.

### Command model

- No arguments and the explicit `shell` command open the `DurinDevTool>` shell.
- Direct commands and shell commands use one command registry for names,
  aliases, operands, options, defaults, help, and request construction.
- Existing build command names and option meanings remain stable unless this
  plan explicitly replaces an obsolete wrapper-only interface.
- Documentation-plan operations use:

  ```text
  plan list
  plan validate
  plan archive
  ```

  Listing scopes, archive query guards, terminal/Markdown formats, completion
  metadata, dry-run defaults, transactional reference repair, and rollback
  retain their existing DocTool semantics.
- Worktree operations use:

  ```text
  worktree add
  worktree prepare
  worktree list
  worktree open
  worktree remove
  ```

- Full checkout initialization uses `setup`.
- Focused third-party preparation uses:

  ```text
  dependency prepare <name> [<name> ...]
  dependency prepare --all
  dependency validate
  ```

  `setup` invokes the same dependency service internally with the required test
  and development dependency selection. There are no per-library batch files.
- Non-interactive commands never pause for a keypress. Destructive operations
  retain their existing explicit confirmation or `--yes`/`--force` contracts.
- Direct commands return nonzero on parse, validation, child-process, or
  operation failure. Styled output remains terminal-sensitive, and `--plain`
  remains the stable Agent output mode.

### Cold-start and interpreter boundary

- DurinDevTool must start before `.venv` exists.
- `DevTool.bat` prefers `.venv\Scripts\python.exe` when available and otherwise
  locates Python 3 through the Windows `py` launcher or `python` on `PATH`.
- Package import and top-level command discovery before setup depend only on the
  Python standard library. Dependency-heavy modules are imported lazily.
- Without `.venv`, `help`, `shell`, `setup`, and safe worktree preparation and
  inspection commands remain available. A build command reports one actionable
  setup error instead of importing an unavailable package.
- A successful interactive `setup` restarts the shell under the newly created
  virtual environment before accepting dependency-backed commands. A direct
  `setup` completes and exits with its operation result.
- Setup ordering remains:
  1. validate that the target is the main checkout;
  2. initialize missing `.agents/build-config.json` without overwriting it;
  3. run the non-mutating prerequisite preflight;
  4. create or repair `.venv` and install pinned requirements;
  5. prepare required third-party dependencies.
- Linked worktrees continue to use `worktree prepare`; `setup` rejects them.

### Ownership and safety

- A checkout continues to have one source/build writer.
- The current checkout-wide build lock, interruption marker, command logs,
  toolchain environment cache, and recovery semantics retain their existing
  locations and behavior.
- Worktree preparation continues to share `.agents`, `.venv`, and
  `Engine/External` through validated Windows junctions.
- Worktree removal validates exact registered targets, detaches only expected
  junctions, and invokes Git only after link safety checks succeed.
- Purge remains restricted to registered preset and generated-output roots.
- Runtime launch continues to track and terminate the complete application
  process tree.
- Moving code must not weaken path containment checks by replacing resolved
  repository paths with caller-controlled working-directory assumptions.

### Cutover and documentation

- Old entrypoints remain untouched only while internal stages are incomplete;
  they are not adapted into compatibility wrappers.
- The final cutover deletes all four old root entrypoints and every migrated
  implementation file in the same stage that switches current documentation
  and tests to DurinDevTool.
- Current documentation describes only the final interface. No migration
  section, legacy spelling note, or compatibility table is added.
- Archived plans are not mechanically rewritten. Current operational
  documents, active plans, investigations that specify live behavior, and CMake
  diagnostics must not direct users to deleted paths.

## Current Foundations and Gaps

- BuildTool is already owned under `Tools/BuildTool` and has a shared
  direct/shell command specification, lazy
  toolchain initialization, stable request objects, structured output,
  checkout locking, recovery, stop, purge, scaffolding, and runtime launch
  behavior. These are foundations to move, not redesign.
- DocTool is already owned under `Tools/DocTool` and provides plan catalog
  parsing, metadata validation, guarded archive discovery, human/Agent output
  formats, and transactional archive application with rollback. These are
  foundations to move under the `plan` family, not redesign.
- WorktreeTool already centralizes registered-worktree discovery, terminal
  opening, shared-link preparation, safe removal, dry-run behavior, and
  system-Python fallback.
- Setup already has the required high-level operation ordering, but the
  orchestration is split across the root batch file and multiple bootstrap
  batch wrappers.
- Bootstrap's Python implementation and third-party manifests are already
  mostly platform-neutral, but their repository-root calculation and manifest
  location depend on their current directory depth.
- The existing Agent tooling suite under `Engine/Scripts/Tests` and DocTool
  suite under `Tools/Tests` directly import separate product packages and
  assert separate wrapper contents. They must move with the product and test
  the unified public interface instead of old paths.
- CMake third-party diagnostics name library-specific setup batch files, and
  `CMake/Config/BuildOptions.cmake` names `BuildTool.bat`.
- Current repository and build documentation contains the old entrypoint
  names. Archived plans also contain them, but are intentionally historical.
- `requirements.txt`, `.venv`, `.agents`, `Build`, `Install`,
  `Engine/External`, project binaries, and project intermediate directories
  remain rooted at the repository level and do not move under `Tools`.

## Implementation Stages

### Stage 0: Baseline and Working-Set Contract

- [x] Record the baseline commit and confirm the checkout has no overlapping
  user changes in the files this plan will modify.
- [x] Inventory the exact files moving from the build, documentation,
  worktree, and bootstrap implementations, including third-party manifests and
  tests.
- [x] Classify every reference to the four old entrypoints or implementation
  paths as current operational behavior, live diagnostic, active-plan
  provenance, test coupling, or archived history.
- [x] Run the existing Agent tooling and DocTool tests and record any baseline
  failures.
- [x] Verify the current direct and interactive help, read-only commands,
  DocTool listing/validation/archive dry run, worktree dry runs, and setup
  preflight behavior used as parity evidence.
- [x] Record the working set, key symbols, path assumptions, and validation
  result in the stage handoff.

#### Acceptance Gate

- The move list and current-reference list are complete enough that deleting an
  old path cannot leave an undiscovered live caller.
- Baseline behavior and any pre-existing failures are recorded before code
  movement begins.
- The next stage can introduce the new package without guessing repository-root
  or interpreter contracts.

#### Stage 0 Handoff

- Baseline commit: `8eac1950`.
- Working set:
  - four root entrypoints: `BuildTool.bat`, `DocTool.bat`,
    `WorktreeTool.bat`, and `Setup.bat`;
  - 17 tracked files under `Tools/BuildTool`, including
    `AgentBuildProfiles.json`, the build package, and scaffolding templates;
  - five tracked files under `Tools/DocTool`;
  - `Engine/Scripts/Utils/worktree_tool.py`;
  - 28 tracked bootstrap scripts and manifests under
    `Engine/Scripts/Bootstrap`;
  - `Engine/Scripts/Tests/test_agent_tooling.py` and
    `Tools/Tests/test_doc_tool.py`;
  - current repository instructions, operational documentation, live
    investigations, active-plan references, CMake diagnostics, and
    `Tools/README.md`;
  - `ProfilingToolService.cpp` and `ProfilingToolServiceTests.cpp`, which read
    Tracy manifests from the bootstrap directory and expose the manifest
    repair command.
- Key symbols:
  - BuildTool `CommandSpec`, `CommandFamilySpec`, `COMMAND_SPECS`,
    `COMMAND_FAMILIES`, `parse_args`, `run_shell`, and `main`;
  - DocTool `_parser`, `_execute`, `run_shell`, `main`, `load_catalog`,
    `preview_archive`, and `apply_archive`;
  - WorktreeTool `get_worktrees`, `prepare_registered_worktree`,
    `validate_directory_links`, `remove_worktree`, `parse_args`, and `main`;
  - bootstrap `ensure_agent_config`, preflight checks, `load_manifests`,
    dependency `run_command`, and setup entrypoints;
  - profiling-service `ToolsManifestPath`, `ClientManifestPath`, and
    manifest-provided `RepairCommand`.
- Reference classification:
  - executable ownership is split across the four root launchers, two
    `Tools` products, and the worktree/bootstrap implementation under
    `Engine/Scripts`;
  - live callers include BuildTool and third-party CMake diagnostics plus the
    editor profiling service's Tracy manifest paths;
  - current documentation, live investigations, and active plans require
    final-cutover updates;
  - archived plans remain historical and are excluded from mechanical
    replacement.
- Decisions: Stage 1 adds only the bootstrap-safe unified product skeleton.
  Existing domain implementations remain authoritative until their owning
  stages, and old launchers are deleted only in the atomic cutover.
- Open questions: none.
- Validation:
  - `python -m unittest Engine.Scripts.Tests.test_agent_tooling
    Tools.Tests.test_doc_tool`: 185 passed, one skipped;
  - BuildTool help, status, presets, and scripted read-only shell: passed;
  - DocTool help, active listing, all-scope validation, archive dry run, and
    scripted shell: passed;
  - Worktree list and terminal-open dry run: passed;
  - setup help and prerequisite preflight: passed;
  - main-worktree `prepare --dry-run` was correctly rejected, but
    `WorktreeTool.bat` returned zero; this is recorded as a baseline launcher
    defect for Stage 1.

### Stage 1: Bootstrap-Safe Product Skeleton

- [x] Add `Tools/DurinDevTool/DevTool.bat` with virtual-environment preference
  and system-Python fallback.
- [x] Add the `durin_dev_tool` package, validated repository-root discovery,
  shared error boundary, top-level parser, lazy command registry, and
  `DurinDevTool>` shell entry.
- [x] Keep package import and command discovery standard-library-only until a
  selected handler requires prepared dependencies.
- [x] Add explicit prepared-environment capability checks and actionable
  failures for dependency-backed commands.
- [x] Implement direct and shell help generation from the same command
  registry.
- [x] Add focused tests for interpreter selection, repository discovery from
  arbitrary working directories, missing `.venv`, help, and shell startup.
- [x] Update the plan status and record the stage handoff.

#### Acceptance Gate

- `DevTool help`, `DevTool shell`, and module import work with a supported
  system Python when `.venv` is absent.
- Invocation works from outside the repository root without deriving a wrong
  workspace.
- A dependency-backed placeholder command fails before importing unavailable
  packages and identifies `DevTool setup` as the recovery action.
- The skeleton introduces no second command specification for shell use.

#### Stage 1 Handoff

- Baseline commit: `efdedb41`.
- Working set:
  - `Tools/DurinDevTool/DevTool.bat`;
  - bootstrap-safe modules under
    `Tools/DurinDevTool/durin_dev_tool`, including the process entrypoint,
    repository discovery, command registry, CLI, shell, and lazy command
    handlers;
  - `Tools/DurinDevTool/tests/test_skeleton.py`;
  - this plan status and handoff.
- Key symbols:
  - `discover_repository_root`, `find_repository_root`, and
    `is_repository_root`;
  - `Capability`, `CommandSpec`, `COMMAND_SPECS`, `CommandRegistry`, and
    `require_prepared_environment`;
  - `run`, `main`, and `run_shell`.
- Decisions:
  - repository discovery is anchored to the installed package path, then
    validates `.git`, `CMakeLists.txt`, `Engine`, and `Tools`, so an unrelated
    caller working directory cannot select a different workspace;
  - the launcher prefers `.venv`, then `py -3`, then `python`, and preserves
    the selected interpreter's exit code;
  - the temporary `build` registration proves the prepared-environment and
    lazy-import boundary, but the existing BuildTool remains authoritative
    until Stage 2;
  - prepared-environment checks validate interpreter identity through the
    filesystem, which supports linked-worktree junctions, then probe only the
    command's declared Python modules before lazily importing its handler;
  - a system-Python invocation with an existing `.venv` tells the caller to
    restart through `DevTool.bat`; an incomplete `.venv` identifies its missing
    packages and directs repair through main-checkout `DevTool setup`;
  - direct and interactive parsing and help use only `COMMAND_SPECS`.
- Open questions: none.
- Validation:
  - combined existing and new Python tooling suite: 196 passed, one skipped;
  - isolated system-Python import, direct help from an unrelated working
    directory, launcher help, and scripted shell help: passed;
  - controlled checkout without `.venv`: dependency-backed `build` returned
    exit code one, named `DevTool setup` and `DevTool worktree prepare`, and
    did not import its handler;
  - Python bytecode compilation for the new package: passed.

### Stage 2: Build Domain Consolidation

- [x] Move the existing `durin_build_tool` implementation into
  `durin_dev_tool/build` using package-relative imports.
- [x] Integrate build, test, run, artifact, stop, purge, preset, and scaffolding
  handlers into the top-level command registry.
- [x] Replace product-facing `BuildTool` names, prompts, usage strings, errors,
  process metadata, and test expectations with `DurinDevTool` or neutral
  build-domain terminology.
- [x] Preserve request defaults, option validation, host-aware shell
  tokenization, output modes, logs, heartbeats, lazy toolchain preparation,
  checkout locking, recovery, stop behavior, and runtime job ownership.
- [x] Preserve the existing `create project` and `create module` transaction
  and rollback semantics.
- [x] Move and reorganize build-domain tests under
  `Tools/DurinDevTool/tests` without reducing behavioral coverage.
- [x] Prove direct/shell request parity for every migrated command.
- [x] Update the plan status and record the stage handoff.

#### Acceptance Gate

- All migrated build-domain tests pass from their new package location.
- Equivalent direct and shell invocations produce equivalent requests.
- Read-only and artifact commands remain toolchain-free.
- Lock, interruption, purge, runtime process, scaffolding rollback, output, and
  error-context tests match their pre-move behavior.
- No migrated module derives the repository root from a fixed parent count.

#### Stage 2 Handoff

- Baseline commit: `4300b486`.
- Working set:
  - build-domain implementation, build profile manifest, and scaffolding
    templates under `Tools/DurinDevTool/durin_dev_tool/build`;
  - the top-level registry, shell session state, and core command handlers;
  - migrated build-domain and unified-registry tests under
    `Tools/DurinDevTool/tests`;
  - this plan status and handoff.
- Key symbols:
  - top-level `CommandSpec`, `COMMAND_SPECS`, `CommandRegistry`,
    `_build_command`, and nested `create` specifications;
  - build `request_from_namespace`, `execute_request`,
    `execute_shell_request`, `create_context`, `derive_context`, and
    `execute_context`;
  - shell `split_windows_command_line`, `split_shell_command`, and
    `normalize_compact_build_command`.
- Decisions:
  - the top-level registry owns every build-domain name, operand, option,
    default, capability, and handler registration; the migrated package
    contains no second parser or command-specification table;
  - the build handler constructs the existing typed `CommandRequest` directly
    from the registry namespace, so direct and shell execution do not reparse
    arguments;
  - the unified shell retains session-local preset selection and a reusable
    lazy toolchain context while direct commands remain isolated;
  - the old BuildTool implementation and root launcher remain untouched until
    the atomic Stage 5 cutover, while the migrated implementation is exercised
    through `DevTool`.
- Open questions: none.
- Validation:
  - combined existing and migrated Python tooling suite: 297 passed, two
    skipped;
  - migrated build-domain, unified-registry, and skeleton suite: 112 passed,
    one skipped;
  - Python bytecode compilation for the unified package: passed;
  - direct `DevTool status`, `presets`, and `preset` smoke tests: passed
    without toolchain initialization;
  - scripted unified shell retained preset selection across a subsequent
    `status` command, and the session reuse test prepared the toolchain once
    across a preset switch.

### Stage 3: Documentation Domain Consolidation

- [x] Move `Tools/DocTool/durin_doc_tool` into
  `durin_dev_tool/documentation` using package-relative imports.
- [x] Register `plan list`, `plan validate`, and `plan archive` in the shared
  top-level command registry and shell.
- [x] Preserve active/completed/archive/all scopes, query filtering,
  `--all-results` guards, terminal and Markdown formats, and color selection.
- [x] Preserve metadata validation for titles, summaries, lifecycle state,
  completion dates, archive months, and direct references.
- [x] Preserve archive preview as the default and require explicit apply for
  repository mutation.
- [x] Preserve archive transaction rollback across plan moves and reference
  repairs.
- [x] Move DocTool tests from `Tools/Tests` into
  `Tools/DurinDevTool/tests` without reducing behavioral coverage.
- [x] Prove direct/shell request and output-default parity for every migrated
  plan command.
- [x] Update the plan status and record the stage handoff.

#### Acceptance Gate

- The migrated documentation-domain tests pass from the unified package.
- `plan list` remains human-oriented in the shell and Markdown-oriented for
  direct Agent invocation unless explicitly overridden.
- Unfiltered archive/all discovery remains rejected without
  `--all-results`.
- Archive preview performs no writes, apply repairs references atomically, and
  injected failures restore every changed file.
- Plan validation results match the pre-move DocTool catalog.

#### Stage 3 Handoff

- Baseline commit: `618743f9`.
- Working set:
  - documentation plan parsing, discovery, rendering, validation, and archive
    transaction services under
    `Tools/DurinDevTool/durin_dev_tool/documentation`;
  - the top-level `plan` command specifications and nested-name normalization;
  - migrated documentation-domain and registry-parity tests under
    `Tools/DurinDevTool/tests`;
  - this plan status, corrected active-plan metadata, and handoff.
- Key symbols:
  - top-level `PLAN_LIST`, `PLAN_VALIDATE`, `PLAN_ARCHIVE`, and
    `DOCUMENTATION_HANDLER`;
  - documentation `run`, `_run_list`, `_run_validate`, `_run_archive`, and
    `_print_archive`;
  - retained `load_catalog`, `parse_plan`, `render_listing`,
    `preview_archive`, and `apply_archive`.
- Decisions:
  - the top-level registry is the only parser and command-specification table
    for documentation commands; the migrated domain contains services and one
    typed handler, not a second CLI or shell;
  - handler execution uses the presence of unified shell session state solely
    to select the historical terminal listing default, while direct invocation
    remains Markdown by default and explicit formats are identical;
  - archive mutation remains opt-in through `--apply`; discovery, reference
    rewriting, atomic writes, validation, and rollback are unchanged;
  - the old DocTool implementation, tests, and root launcher remain untouched
    until the Stage 5 atomic cutover;
  - active-plan `Completed:` metadata remains empty; completed stage numbers
    belong in Current Status and handoffs rather than the lifecycle date field.
- Open questions: none.
- Validation:
  - combined existing and migrated Python tooling suite: 311 passed, two
    skipped;
  - migrated build, documentation, unified-registry, and skeleton suite: 126
    passed, one skipped;
  - Python bytecode compilation for the unified package and tests: passed;
  - old DocTool and unified DevTool catalog validation/list/archive-preview
    smoke tests produced identical output: 9 active, zero completed, and 23
    archived plans;
  - scripted unified shell validation and listing passed, with human-oriented
    terminal output selected by default;
  - archive preview/apply routing and injected-failure rollback tests passed.

### Stage 4: Setup, Dependency, and Worktree Consolidation

- [x] Move worktree implementation into `durin_dev_tool/worktree` and register
  its five canonical subcommands.
- [x] Move Agent configuration initialization, prerequisite preflight, Python
  environment setup, third-party setup, and manifests into
  `durin_dev_tool/bootstrap`.
- [x] Replace bootstrap batch orchestration with typed Python services and
  subprocess boundaries that preserve exit codes and stream useful output.
- [x] Implement `setup`, `dependency prepare`, and `dependency validate`
  handlers against those shared services.
- [x] Update the editor profiling service and its native tests to resolve Tracy
  manifests from DurinDevTool and advertise the canonical focused dependency
  repair command.
- [x] Remove unconditional pause behavior from setup.
- [x] Preserve idempotence, non-overwrite of local Agent configuration,
  preflight-before-mutation ordering, pinned requirement installation, and
  complete dependency preparation.
- [x] Make `worktree prepare` call the shared Python preflight service rather
  than an old batch path.
- [x] Preserve system-Python operation for the no-`.venv` setup and worktree
  paths.
- [x] Restart an interactive shell under `.venv` after setup succeeds.
- [x] Port setup, preflight, manifest, dependency, worktree, junction, dry-run,
  and removal-safety tests to the new package.
- [x] Update the plan status and record the stage handoff.

#### Acceptance Gate

- A simulated fresh checkout can discover help, reject invalid prerequisites
  before mutation, create the Python environment, and continue through the
  dependency service without importing uninstalled packages early.
- Setup remains idempotent and never overwrites an existing
  `.agents/build-config.json`.
- Linked-worktree setup is rejected and points only to
  `DevTool worktree prepare`.
- Worktree preparation and removal safety tests pass unchanged in substance.
- Every third-party manifest validates from its new location, and focused
  dependency selection replaces every library-specific setup wrapper use case.
- The profiling service resolves the relocated Tracy client/tool manifests and
  reports a runnable DurinDevTool repair command.

#### Stage 4 Handoff

- Baseline commit: `e92cd43d`.
- Working set:
  - bootstrap-safe Agent configuration, prerequisite, Python environment,
    dependency, manifest, setup, and handler services under
    `Tools/DurinDevTool/durin_dev_tool/bootstrap`;
  - worktree lifecycle, junction safety, shared preflight, and handler services
    under `Tools/DurinDevTool/durin_dev_tool/worktree`;
  - top-level setup, dependency, and five-leaf worktree command
    specifications, plus shell restart termination state;
  - relocated Tracy manifest consumers in the editor profiling service and
    its native tests;
  - migrated bootstrap/worktree tests and this plan handoff.
- Key symbols:
  - bootstrap `setup_repository`, `ensure_python_environment`,
    `validate_prerequisites`, `DependencyRequest`, `prepare_dependencies`, and
    `validate_repository_manifests`;
  - worktree `repository_paths`, `prepare_registered_worktree`,
    `run_preflight`, `validate_directory_links`, `detach_link`, and
    `remove_worktree`;
  - registry `DEPENDENCY_PREPARE`, `DEPENDENCY_VALIDATE`, `WORKTREE_OPEN`,
    `WORKTREE_LIST`, `WORKTREE_ADD`, `WORKTREE_PREPARE`, and
    `WORKTREE_REMOVE`.
- Decisions:
  - every migrated domain remains standard-library-only until its handler is
    invoked, so setup and worktree commands operate through system Python
    before `.venv` exists;
  - setup validates prerequisites before any repository mutation, then creates
    the Agent config only when absent, prepares the pinned Python environment,
    and prepares all ordinary, test, and development dependencies;
  - dependency preparation requires exactly one of `--all` or `--libs`;
    library-specific wrappers are represented by focused `--libs` selections;
  - worktree mutation and removal safety logic is retained, while preflight
    calls the shared Python service rather than a batch wrapper;
  - a system-Python interactive shell launches the prepared `.venv` shell
    after successful setup and then terminates its original shell loop;
  - the old implementation trees and root launchers remain untouched until
    the Stage 5 atomic cutover.
- Open questions: none.
- Validation:
  - combined existing and migrated Python tooling suite: 351 passed, two
    skipped;
  - migrated-domain and unified-registry suite: 166 passed, one skipped;
  - system-Python help, dependency-manifest validation, and worktree listing:
    passed without dependency-backed imports;
  - all 10 relocated third-party manifests validated, and the Tracy repair
    command resolves to focused `dependency prepare --libs tracy,tracy-tools`;
  - full `all` build for `Win64-Debug-DurinEditor-Tests`: passed;
  - focused `FProfilingToolServiceTests.*`: five passed;
  - unfiltered `EngineTests` built successfully but later hit the pre-existing
    unrelated fatal assertion `Mount registry is immutable after publication`;
    this test-process failure required no recovery and the focused affected
    suite passed.

### Stage 5: Atomic Repository Cutover

- [x] Update `AGENTS.md`, `README.md`, current build/bootstrap/tooling
  documentation, live investigations, active-plan references, and CMake
  diagnostics to the canonical DurinDevTool commands and paths.
- [x] Update test discovery and any repository automation to load tests from
  `Tools/DurinDevTool/tests`.
- [x] Delete root `Setup.bat`, `BuildTool.bat`, `DocTool.bat`, and
  `WorktreeTool.bat`.
- [x] Delete the migrated `Tools/BuildTool`, `Tools/DocTool`,
  `Engine/Scripts/Utils/worktree_tool.py`, and `Engine/Scripts/Bootstrap`
  implementation and wrapper files.
- [x] Remove empty implementation directories left by the move when they have
  no separate engine-owned purpose.
- [x] Confirm current source and documentation contain no calls to deleted
  entrypoints or implementation paths, excluding archived historical plans.
- [x] Do not add redirects, forwarding wrappers, legacy command aliases,
  migration notes, or duplicate manifests.
- [x] Update the plan status and record the stage handoff.

#### Acceptance Gate

- The only supported development-tool launcher is
  `Tools/DurinDevTool/DevTool.bat`.
- No live code, current documentation, active plan, or diagnostic tells the
  user to invoke a deleted entrypoint.
- No duplicate implementation or third-party manifest remains in the old
  directories.
- Root-directory clutter is reduced without moving repository-level build
  configuration or output ownership under `Tools`.

#### Stage 5 Handoff

- Baseline commit: `09b67b2e`.
- Working set:
  - repository instructions, root and Tools READMEs, current build/bootstrap/
    tooling/runtime/workspace documentation, live investigations, and active
    plan references;
  - CMake code-model and third-party dependency diagnostics;
  - unified test discovery under `Tools/DurinDevTool/tests`;
  - deletion of the four root launchers, the old build/documentation products,
    bootstrap implementation and manifests, worktree implementation, and old
    test modules;
  - this plan status, checklist, and handoff.
- Key decisions:
  - `Tools/DurinDevTool/DevTool.bat` is the only supported Windows development
    launcher; no wrappers, aliases, duplicate manifests, or migration notes
    remain;
  - current documentation and diagnostics use typed `setup`, `dependency`,
    `worktree`, build, and `plan` commands while archived plans remain
    unchanged historical evidence;
  - test discovery runs only the self-contained unified suite; an unused set
    of legacy imports found by deletion validation was removed.
- Open questions: none.
- Validation:
  - DurinDevTool Python discovery: 166 passed, one skipped;
  - Python compilation, system-Python help, dependency validation, worktree
    listing, and launcher `worktree open --dry-run`: passed;
  - plan validation: 9 active, 0 completed, and 23 archived plans passed;
  - stale live entrypoint and old implementation-path scan: no matches;
  - full `all` build for `Win64-Debug-DurinEditor-Tests`: passed;
  - focused `FProfilingToolServiceTests.*`: five passed.

### Stage 6: Clean-Checkout and End-to-End Validation

- [x] Run the complete DurinDevTool Python test suite.
- [x] Validate all active and archived plan metadata through
  `DevTool plan validate --scope all`.
- [x] Exercise system-Python `help`, missing-environment diagnostics, setup
  preflight, and worktree preparation through controlled no-`.venv` fixtures.
- [x] Exercise direct `status`, `presets`, dependency manifest validation, and
  a scripted read-only shell through the new launcher.
- [x] Exercise worktree add/prepare/remove validation and dry-run paths without
  bypassing DurinDevTool.
- [x] Follow the repository build instructions and use DurinDevTool to
  configure as needed, build target `all`, and run the required native tooling
  tests.
- [x] Run formatting, import, stale-reference, and diff checks.
- [x] Update current operational documentation with any lasting behavior
  discovered during validation.
- [x] Mark every evidence-backed checklist item complete, record final
  validation in Current Status, and archive the plan only after all required
  gates pass.

#### Acceptance Gate

- The full Python tooling suite, plan validator, direct/shell smoke tests, and
  full `all` build succeed through the new entrypoint.
- Fresh-checkout and linked-worktree recovery guidance references only commands
  that exist.
- There are no stale imports, wrapper paths, package-depth assumptions, or live
  operational references to deleted tooling.
- The repository is left with one coherent development interface and no
  compatibility residue.

#### Stage 6 Handoff

- Baseline commit: `f951539d`.
- Working set:
  - the complete DurinDevTool Python suite and focused cold-start, setup,
    dependency, worktree, and command-registry fixtures;
  - direct and scripted-shell launcher smoke tests;
  - registered-worktree validation and dry-run operations;
  - the selected Agent Build Profile's full build and native profiling-tool
    service tests;
  - this plan's completion metadata, final checklist, handoff, and archive.
- Key decisions:
  - controlled fixtures provide the destructive and no-`.venv` clean-checkout
    coverage without modifying the prepared main checkout;
  - live worktree validation uses a rejected conflicting-mode add request plus
    prepare/remove dry runs against an existing registered linked worktree;
  - no operational-document update is required because validation found no
    lasting behavior missing from the owning documentation.
- Open questions: none.
- Validation:
  - DurinDevTool Python discovery: 166 passed, one platform-dependent skip;
  - focused system-Python, missing/incomplete-environment, launcher selection,
    setup preflight/orchestration, and worktree safety fixtures: 34 passed;
  - system-Python help, direct status/presets/dependency validation/worktree
    inspection, and scripted read-only shell: passed;
  - plan validation before completion: 9 active, 0 completed, and 23 archived
    plans passed;
  - worktree add argument validation and registered linked-worktree
    prepare/remove dry runs: passed;
  - full `all` build for `Win64-Debug-DurinEditor-Tests`: passed;
  - focused `FProfilingToolServiceTests.*`: five passed;
  - Python compilation, stale live reference/import scan, and diff checks:
    passed.
  - archive application and final plan validation: 8 active, 0 completed, and
    24 archived plans passed.

## Validation Matrix

| Area | Required Evidence |
| --- | --- |
| Package startup | Import, help, and shell startup under system Python with no `.venv` |
| Interpreter selection | `.venv` preference, `py -3` fallback, `python` fallback, and actionable no-Python failure |
| Repository discovery | Correct root from repository root, product directory, linked worktree, and unrelated current directory |
| Command parity | Table-driven direct/shell request equality for every command and supported option |
| Plan operations | Catalog scopes, filters, output defaults, metadata validation, archive preview/apply, reference repair, and rollback |
| Setup | Main-worktree restriction, config non-overwrite, preflight ordering, venv preparation, idempotence, and shell restart |
| Dependencies | Manifest validation, all/default selection, focused multi-library selection, test/development selection, and configuration handling |
| Build ownership | Lock exclusivity, environment caching, interruption marker, recovery, stop, and process-tree behavior |
| Build operations | Configure, build, test, clean, rebuild, purge, run, presets, status, artifact access, and scaffolding |
| Output | Interactive styling, redirected/plain output, compact/full/progress modes, logs, diagnostics, and exit codes |
| Worktrees | Discovery, add, prepare, list, open, junction validation, dry run, dirty/locked refusal, and safe removal |
| Cutover | Deleted old entrypoints, deleted old modules, no duplicate manifests, and no live stale references |
| End-to-end | New launcher performs read-only smoke tests, dependency validation, full `all` build, and required native tests |
| Documentation | Current owning docs and active plans use only the final interface; plan metadata validation passes |

## Definition of Done

- Every implementation-stage acceptance gate passes.
- `Tools/DurinDevTool/DevTool.bat` is the sole supported Windows development
  entrypoint.
- The Python implementation and its tests are owned under
  `Tools/DurinDevTool`.
- Human shell and Agent direct commands share one command model and execution
  semantics.
- Setup works from a fresh main checkout, and worktree preparation works before
  a linked worktree has a local virtual-environment path.
- Existing build, recovery, purge, runtime-process, and worktree-link safety
  contracts remain covered and passing.
- Root `Setup.bat`, `BuildTool.bat`, `DocTool.bat`, and `WorktreeTool.bat` and
  all migrated old implementation paths are deleted.
- No compatibility wrappers, transition aliases, migration documentation, or
  duplicated implementation remains.
- Current documentation and live diagnostics describe the lasting interface.
- DurinDevTool completes a successful full `all` build through the selected
  Agent Build Profile.
- The completed plan is archived according to the repository plan lifecycle.

## Deferred Follow-ups

- Command completion and persistent shell history.
- Machine-readable JSON output for status or other discovery commands.
- Global installation, PATH registration, or a shorter repository-root
  launcher.
- A richer terminal UI or background-operation model.
- Cross-platform launchers beyond the direct Python-module path where current
  repository support does not already require them.

## Related Documentation

- `AGENTS.md`
- `README.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/ThirdPartyBootstrap.md`
- `Documentation/Development/Tooling/IDECodeModel.md`
- `Documentation/Investigations/DurinDevToolWindowsLockRecovery.md`
- `Documentation/Plans/RuntimeVariantAndTracyProfiling.md`

## Related Code

- `Tools/DurinDevTool/DevTool.bat`
- `Tools/DurinDevTool/durin_dev_tool`
- `Tools/DurinDevTool/tests`
- `Engine/Source/Editor/MainFrame/Private/ProfilingToolService.cpp`
- `Engine/Tests/Native/EngineTests/Private/ProfilingToolServiceTests.cpp`
- `CMake/Config/BuildOptions.cmake`
- `Engine/CMake/ThirdParty`
- `requirements.txt`
