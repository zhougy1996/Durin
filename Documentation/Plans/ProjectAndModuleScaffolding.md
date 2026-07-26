# Project And Module Scaffolding Plan

Summary: Transactional BuildTool workflows for module creation, workspace project creation, descriptor/CMake registration, and typed project launch.

Last reviewed: 2026-07-26

## Current Status

Stages 0 through 3 are complete. BuildTool now creates runtime and editor
modules and complete workspace-local projects from versioned disk templates,
updates descriptor and root CMake registration transactionally, validates the
final workspace descriptor graph, and supports mutation-free dry runs. Stage 4
typed project launch and final integration is the next delivery target.

## Goal

Reduce routine module and workspace-project setup to one discoverable BuildTool
command that produces repository-conforming files, updates every required
descriptor and CMake registration, and either completes as a coherent change or
leaves the workspace unchanged.

The intended canonical workflows are:

```powershell
.\BuildTool create module Gameplay --project Sandbox\Sandbox.dproject --kind runtime --private-dependency Core --private-dependency Engine
.\BuildTool create module SceneEditor --project Sandbox\Sandbox.dproject --kind editor --private-dependency DurinEd
.\BuildTool create project MyGame --path MyGame
.\BuildTool run --project MyGame\MyGame.dproject
```

Every direct command must have equivalent interactive-shell syntax.

## Scope

- Add a `create` BuildTool command family with `module` and `project`
  subcommands.
- Resolve project descriptor paths relative to the workspace root and accept
  absolute paths where the selected operation permits them.
- Create a minimal compilable module source tree, `.dmodule`, and
  `CMakeLists.txt`.
- Register new modules in the owning `.dproject` and enable them for the
  selected profile set.
- Create a workspace-local game project with a same-named runtime module,
  descriptor, project CMake entrypoint, setup script, content/config roots, and
  root-workspace registration.
- Provide deterministic defaults for runtime and editor modules while allowing
  explicit link type, PCH, dependency, and enablement overrides.
- Add `--dry-run` output that reports planned file creations and modifications
  without writing.
- Add `--project` as a typed `BuildTool run` option and forward it to the
  launcher using the runtime project-selection contract.
- Validate generated descriptors with the same schema and dependency rules used
  by DurinHeaderTool before committing any mutation.
- Cover direct CLI, interactive shell, filesystem planning, rollback, descriptor
  updates, generated content, and end-to-end configure/build behavior.
- Document the lasting command workflow and workspace registration rules after
  implementation.

## Non-Goals

- Installed-engine project generation or building a project outside the current
  workspace.
- Automatic project discovery that replaces explicit root
  `add_subdirectory(...)` registration.
- An editor GUI wizard, IDE integration, or OS shell integration.
- Generating gameplay systems, reflected types, assets, levels, tests, or
  third-party dependency setup.
- Inferring dependencies by scanning C++ includes.
- Renaming, moving, cloning, or deleting existing modules or projects.
- Making `run` configure or build missing artifacts.
- Changing the workspace-global `DURIN_PROFILE_NAME` model.
- Supporting arbitrary user-authored template packages in the first version.

## Design Decisions and Invariants

- `.dproject` and `.dmodule` remain the authoritative project/module
  descriptions. Generated CMake and C++ files do not introduce a second registry
  or hidden state.
- The canonical interface is `BuildTool create module ...` and
  `BuildTool create project ...`. Direct CLI and interactive-shell parsing use
  the shared BuildTool command specification and produce one typed request
  model.
- `create module` requires a project descriptor and a module name. Names must be
  valid C++ identifiers, valid CMake target names, case-insensitively unique
  across all workspace-supplied project descriptors, and equal to the
  `ModuleName` written to the descriptor.
- `--kind runtime` creates `Source/Runtime/<ModuleName>` and enables the module
  in `BaseModules` by default. `--kind editor` creates
  `Source/Editor/<ModuleName>` and enables it in
  `ExtraModules.DurinEditor.Modules` by default. `--enable none`,
  `--enable base`, and repeated `--enable <ProfileName>` explicitly override
  those defaults.
- Module dependencies are supplied with repeatable
  `--public-dependency`, `--private-dependency`,
  `--optional-public-dependency`, and `--optional-private-dependency` options.
  Every named dependency must resolve from the complete workspace project set;
  a module cannot depend on itself.
- Shared linkage is the default. `--link shared|static` and `--pch <Name>`
  override linkage and PCH; omitted PCH uses the descriptor contract's `Self`
  default instead of copying a project-specific shared PCH by guesswork.
- The minimal module template contains a private module entry point and a public
  API-macro header. Generated repository-owned C++ follows the current coding
  standards and does not copy existing class-level export-macro violations.
- Templates live under the BuildTool source tree, are reviewed and versioned
  with the generator, use explicit variables, and contain no host-specific
  absolute paths.
- `create project` is workspace-local in the first version. The target path must
  remain within the workspace, must not overlap an existing project, and is
  registered with a root `add_subdirectory(...)` entry as part of the same
  transaction.
- A new project contains a same-named runtime module enabled in `BaseModules`.
  This makes the generated project immediately meaningful and buildable rather
  than producing an empty descriptor with no validation path.
- Planning is read-only. Before writing, the tool resolves paths, loads every
  relevant descriptor, validates names/dependencies/options, renders all new
  content in memory, and computes exact JSON/CMake edits.
- Mutation is transactional at the tool level. Existing files are backed up
  before replacement, new files are tracked, and any write or validation
  failure restores prior bytes and removes only files created by that
  invocation. The tool never removes or rewrites an unrelated path.
- Existing JSON key order and indentation are preserved where practical;
  generated descriptors use four-space JSON indentation and a final newline.
  Module maps and enablement lists retain existing entries and append the new
  module once.
- `--dry-run` performs full discovery, rendering, and validation, reports paths
  relative to the workspace where possible, and performs no directory creation,
  temporary-file creation, or descriptor mutation.
- Repeating an identical successful request fails with a concise
  already-exists result rather than silently overwriting files. A partially
  matching destination reports every conflict found before any write.
- `BuildTool run --project <descriptor>` validates that the descriptor exists
  and has the `.dproject` extension, normalizes it to an absolute path, and
  passes one `--project=<path>` launcher argument before user `--args`.
  Supplying another project selection through `--args` is rejected as
  ambiguous.
- Scaffolding commands do not require CMake or compiler environment
  initialization. Optional post-creation build validation is deferred; normal
  builds continue to use the repository BuildTool workflow.

## Current Foundations and Gaps

- `Engine.dproject` and `Sandbox.dproject` already define `ModuleDirs`,
  `BaseModules`, and profile-specific `ExtraModules`, but authors currently edit
  those collections manually.
- Per-module CMake entrypoints are uniform
  `add_durin_module(<ModuleName>)` files, and ordinary sources are already
  discovered with `CONFIGURE_DEPENDS`.
- DurinHeaderTool owns the current Python representation of project and module
  descriptors, dependency traversal, cross-project module lookup, and active
  profile enablement.
- BuildTool already has a shared command specification, typed
  `CommandRequest`, direct/shell parity tests, lazy toolchain initialization,
  workspace-root resolution, and structured errors.
- BuildTool currently has only a flat action parser. It needs a command-family
  representation that preserves generated help and direct/shell parity without
  special-casing `create` in two dispatch paths.
- BuildTool has no reusable descriptor edit layer, filesystem transaction, or
  template renderer.
- Root workspace project registration is explicit in `CMakeLists.txt`; no
  generated registry or discovery manifest exists.
- Runtime project selection already accepts `--project=<path>` and
  `--project <path>`, while BuildTool `run` currently exposes only the untyped
  `--args` remainder.
- The editor already supports project browsing and relaunch, but it does not
  create project source/build structure.

## Implementation Stages

### Stage 0: Command and Descriptor Contract

- [x] Extend the shared command specification to represent the `create` command
  family and its `module` and `project` leaf commands.
- [x] Add typed request fields for create kind, name, project/path, linkage, PCH,
  dependency groups, enablement targets, and dry-run state.
- [x] Define one normalized request for equivalent direct and shell syntax,
  including repeated options, Windows paths, and command-family help.
- [x] Introduce descriptor data models and validation errors shared by
  scaffolding logic; reuse DurinHeaderTool schema behavior or extract a common
  schema module without importing DHT process-global caches into BuildTool.
- [x] Build temporary-workspace fixtures representing Engine/Sandbox ownership,
  cross-project dependencies, profiles, and common descriptor failures.
- [x] Add parser and request-model tests before filesystem mutation is
  implemented.

#### Acceptance Gate

- Direct and interactive invocations normalize to equivalent typed requests.
- Top-level, `create`, `create module`, and `create project` help expose one
  canonical syntax and all supported defaults.
- Descriptor validation produces deterministic errors for malformed JSON,
  missing required fields, duplicate project/module names, missing dependencies,
  self-dependencies, and invalid enablement profiles.
- All existing Agent tooling tests and new contract tests pass.

### Stage 1: Read-Only Planning and Transactional Writes

- [x] Implement workspace project discovery from top-level `.dproject` files and
  cross-check the owning project's root CMake registration without introducing
  a global registry file.
- [x] Implement path containment, case-insensitive collision, existing-file,
  existing-CMake-target, and destination-overlap checks.
- [x] Add deterministic template rendering for module descriptors, module entry
  points, API headers, module CMake files, project descriptors, and project
  CMake setup.
- [x] Represent each operation as an ordered plan of directory creations, file
  creations, and exact existing-file replacements.
- [x] Implement stable human-readable dry-run output and `--plain` behavior.
- [x] Implement temporary-file replacement, backup/restore, created-path
  tracking, and rollback on injected failures at every write boundary.
- [x] Reparse all affected descriptors and CMake registration edits before
  finalizing the transaction.
- [x] Add unit tests proving dry-run purity, byte preservation, rollback, and
  refusal to modify paths outside the allowed workspace/project roots.

#### Acceptance Gate

- Dry-run reports the complete eventual mutation set and leaves a before/after
  workspace snapshot byte-identical.
- Every injected write or validation failure restores all pre-existing files and
  removes only invocation-created paths.
- Conflicts are fully reported before the first mutation.
- Generated content is deterministic across repeated plans on Windows and the
  supported non-Windows path model.

### Stage 2: One-Command Module Creation

- [x] Implement runtime/editor path selection and default enablement.
- [x] Generate the module directory, `.dmodule`, `CMakeLists.txt`, minimal module
  entry point, API header, and self-PCH header when using the default PCH mode.
- [x] Update `ModuleDirs` and the selected `BaseModules`/`ExtraModules` roots
  without reordering or duplicating existing entries.
- [x] Validate all dependency categories across project boundaries and preserve
  their public/private and required/optional distinctions in the descriptor.
- [x] Add focused tests for default runtime/editor modules, static/shared
  linkage, self PCH, explicit PCH, no enablement, multiple profiles, and every
  dependency category.
- [x] Run DurinHeaderTool preparation against generated module fixtures.
- [x] Configure and build representative generated runtime and editor modules
  through the documented BuildTool workflow.
- [x] Document module creation, option defaults, dry-run, and recovery behavior.

#### Acceptance Gate

- Each documented module example completes with one command and no manual file
  or descriptor edit.
- DurinHeaderTool resolves the generated module, ownership, enablement, and
  transitive dependency graph for every applicable profile.
- A normal BuildTool configure discovers the new CMake target and representative
  generated runtime/editor modules compile and link.
- Invalid names, dependencies, profiles, and destination conflicts leave the
  workspace unchanged and provide actionable errors.

### Stage 3: One-Command Workspace Project Creation

- [x] Implement workspace-contained project path validation and case-insensitive
  project-name uniqueness.
- [x] Generate `<ProjectName>.dproject`, `CMakeLists.txt`,
  `CMake/<ProjectName>Setup.cmake`, `Configs/`, `Content/`, and the same-named
  runtime module through the Stage 2 scaffolding path.
- [x] Register the project once in the root `CMakeLists.txt` while preserving
  existing project ordering and unrelated formatting.
- [x] Reject paths that are outside the workspace, contain an existing project,
  overlap engine-owned roots, or cannot be expressed safely as a root CMake
  subdirectory.
- [x] Add project-level dry-run, rollback, duplicate-name, duplicate-path, root
  CMake edit, and generated-module integration tests.
- [x] Configure and build a generated project in a disposable workspace fixture
  through BuildTool.
- [x] Document project creation and the workspace-local limitation.

#### Acceptance Gate

- One command creates a registered project whose descriptor, CMake entrypoint,
  content/config roots, and initial module require no manual repair.
- Root configuration discovers both the project and initial module exactly once.
- The generated project builds under `DurinEditor` and `DurinGame` profiles.
- A failed project creation restores the root `CMakeLists.txt` byte-for-byte and
  removes only the newly planned project tree.

### Stage 4: Typed Project Launch and Final Integration

- [ ] Add `--project <descriptor>` to the shared direct/shell `run` command
  model.
- [ ] Normalize and validate the descriptor without initializing the toolchain,
  then forward one unambiguous launcher project argument.
- [ ] Preserve arbitrary non-project launcher arguments after the typed project
  argument and reject conflicting project selectors in `--args`.
- [ ] Add direct/shell request parity, spaces/unicode path, missing descriptor,
  wrong extension, conflicting selector, and process-command tests.
- [ ] Exercise create-module, create-project, configure, build, and typed run as
  one Windows end-to-end workflow.
- [ ] Run the complete Agent tooling suite and the full native `all` build using
  the repository BuildTool workflow.
- [ ] Move lasting CLI behavior into BuildTool operational documentation and
  lasting project/module ownership rules into workspace documentation.

#### Acceptance Gate

- `BuildTool run --project <path>` launches the selected existing project in
  both direct and interactive use without requiring `--args --project=...`.
- Paths containing spaces and non-ASCII characters reach the runtime unchanged.
- The complete scaffolding workflow passes the Agent tooling suite and a full
  native build.
- Operational and ownership documentation describe the landed behavior without
  relying on this plan as a competing specification.

## Validation Matrix

| Area | Required Evidence |
| --- | --- |
| Shared CLI | Direct/shell parity for both create leaves, defaults, repeated options, aliases, invalid operands, and generated help |
| Descriptor schema | Valid/malformed project and module JSON, required fields, all dependency categories, profiles, duplicate names, and cross-project lookup |
| Path safety | Relative/absolute paths, separators, spaces, unicode, case-only collisions, containment, overlap, symlinks/reparse points, and existing destinations |
| Planning | Deterministic action ordering, complete conflict reporting, stable styled/`--plain` dry-run output, and no filesystem mutation |
| Transactions | Injected failures before and after every write, exact backup restoration, created-path cleanup, and unrelated-file preservation |
| Module generation | Runtime/editor defaults, linkage, PCH, API macro, C++ standards, CMake target, enablement, and DHT preparation |
| Project generation | Descriptor, setup script, content/config roots, initial module, root CMake registration, and both active profiles |
| Project launch | Direct/shell syntax, normalized descriptor argument, application arguments, conflicting selectors, and no toolchain initialization |
| Integration | Disposable-workspace create/configure/build tests plus a Windows create-to-run smoke workflow |
| Regression | Complete Agent tooling suite and full native `all` build after the final stage |
| Documentation | BuildTool commands and workspace ownership documents match accepted syntax, defaults, limitations, and failure behavior |

Use the repository setup, build, test, timeout, and single-writer workflow
documented in `Documentation/Development/Build/BuildAndRun.md`.

## Definition of Done

- Every implementation-stage acceptance gate passes with recorded evidence.
- A runtime module, editor module, and workspace project can each be created by
  one documented BuildTool command without follow-up file edits.
- Dry-run is mutation-free and every failed mutation restores the exact prior
  workspace state.
- Generated descriptors are accepted by DurinHeaderTool and generated targets
  build under every profile in which they are enabled.
- Direct and interactive BuildTool interfaces remain equivalent and
  discoverable.
- Typed project launching works without raw launcher-argument knowledge.
- The complete Agent tooling suite and final full native build pass.
- Lasting behavior is documented in the owning Development and Workspace
  documents before this plan is archived.

## Deferred Follow-ups

- Installed-engine and external game-root project generation.
- Automatic workspace project discovery or a dedicated project registry that
  removes root `CMakeLists.txt` edits.
- Editor and IDE project/module creation wizards built on the same transaction
  layer.
- User-authored or plugin-provided template packs.
- Test-module, program-target, plugin, and third-party integration templates.
- Module/project rename, move, clone, disable, and removal operations.
- Optional `--build` or `--run` chaining after successful creation.
- Machine-readable JSON plan output for IDE integrations.

## Related Documentation

- `Documentation/Workspace/WorkspaceProjects.md`
- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/Profiles.md`
- `Documentation/Development/Standards/CodingStandards.md`

## Related Code

- `BuildTool.bat`
- `CMakeLists.txt`
- `CMake/DurinWorkspaceSetup.cmake`
- `CMake/Project/ProjectSetup.cmake`
- `CMake/Project/ProjectTargets.cmake`
- `Engine/Scripts/Build/durin_build_tool/cli.py`
- `Engine/Scripts/Build/durin_build_tool/config.py`
- `Engine/Scripts/Build/durin_build_tool/core.py`
- `Engine/Scripts/Tests/test_agent_tooling.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/project_config.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/module_config.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/`
