# DurinDevTool Architecture Simplification Plan

Summary: Remove implicit repository state, compatibility facades, duplicated protocol validation, and cross-domain command orchestration while splitting DurinDevTool into independently owned functional modules.

Last reviewed: 2026-08-13

Status: Archived
Completed: 2026-08-13

## Current Status

Stages 0 through 8 are complete. The CLI now constructs one
immutable `RepositoryContext` containing the resolved checkout root and tracked
configuration, pairs it with explicit `CommandIO`, and passes both through
registry execution and the interactive shell. Build-context creation consumes
the supplied repository configuration paths, and all production domain errors
now derive from `DevToolError`; bootstrap, worktree, and documentation handlers
no longer replace those errors with string-only wrappers. Bootstrap and
worktree services now consume explicit repository and command-I/O values, their
handlers no longer redirect process-global streams, and both legacy
`repository_paths()` scopes have been removed. Compatibility overloads for
direct service tests resolve compatibility defaults only at call time. Build
profiles, presets, local configuration, state, locks, logs, runtime locations,
scaffolding templates, and descriptor schemas now derive from explicit
repository context in production and no longer resolve the checkout during
module import. Two repository contexts produce isolated build paths in one
process, and import characterization rejects any build module that loads a
repository context during import. Stage 2 has moved the command model plus the
core, build/test/scaffolding, asset, scene, documentation,
bootstrap/dependency, and worktree
declarations into command-infrastructure and feature-owned spec modules. The
registry now caches its parser after feature filtering, and characterization
proves spec imports do not eagerly load their handlers. Build dispatch remains
typed by `Action`; bootstrap, worktree, and documentation adapters now use
bounded dispatch tables instead of long action chains. The completed Stage 2
suite passes 350 tests in 12.91 seconds. Stage 3 has begun by moving build
profiles, presets, local configuration, environment-provider, output-mode, and
test-mode value types into a dedicated build models module while retaining
explicit compatibility re-exports during consumer migration. Repository/user
build configuration and profile/preset manifests now decode through an
explicit-path config-I/O module; selection, cache decoding, and preset path
expansion have a separate selection owner.
The build command adapter now constructs separate immutable requests for
configure, build, rebuild, test, run, purge, location, module creation, and
project creation. The former action/options union and unrelated fallback
properties have been removed from production; direct service tests use a
visibly test-only constructor until their Stage 7 owner-module split.
Production context and output paths discriminate concrete request types.
Interactive build
state now lives in a typed `BuildSession`, and concrete requests declare their
own toolchain requirement; the central action-set exception logic is gone.
`BuildContext` and pure context creation/derivation now have a separate owner;
production orchestration prepares toolchain state only after context selection
and only for requests that require it. The completed Stage 3 suite passes 350
tests.
Stage 4 is complete. The shared toolchain module is the sole implementation
of environment-key casing, PATH command lookup, Visual Studio discovery,
setup-script capture, and environment output parsing. Build-specific Visual
Studio environment caching, diagnostic probing, required-command policy, and
context preparation now live in a bounded toolchain-context module; production
orchestration no longer reaches those services through `build.core`. Bootstrap
toolchain selection now has its own service, while preflight is limited to host
prerequisite and minimum-version validation. Build and bootstrap callers use
the shared discovery/capture primitives directly, with no same-named forwarding
functions or string-only error translation.
Descriptor, repository, user-config, and asset-report decoding now share one
cached JSON Schema service with duplicate-key rejection and JSON-path
diagnostics. Their Python validators retain only repository containment and
cross-field/order/state/count semantics.
Runtime selection, project resolution, executable location, and process policy
now have a shared runtime-program service. Ordinary runtime launches, scene
authoring, and production asset commands use the same logged process engine,
including cancellation, timeout, heartbeat, excerpts, and full-log paths.
The completed Stage 4 suite passes 351 tests. Stages 5 and 6 split bootstrap,
worktree, and documentation lifecycle ownership into focused application,
domain, infrastructure, and adapter modules. Stage 7 has realigned the build,
bootstrap, and worktree tests with those owners, removed the obsolete
bootstrap dependency and worktree service facades, and added dependency-
direction characterization. Stage 8 then removed the expired compatibility
surfaces and replaced coarse capability gates with explicit dependencies. The
final complete suite and all-document/all-plan validation pass.

DurinDevTool currently
contains approximately 14,570 lines of production Python and 4,970 lines of
Python tests. Fourteen production files exceed 400 lines; the largest
concentrate command metadata, build configuration and requests, build
orchestration, worktree workflows, dependency preparation, and documentation
dispatch.

The tool has already extracted several useful services, including build
locations, process execution, locking, recovery, runtime execution, descriptor
validation, and a generic documentation workspace. The remaining architecture
still routes those services through broad facades, action-tagged request
unions, and centralized command metadata.

This plan first freezes the command contract, then makes repository context and
I/O explicit, moves command ownership to feature domains, separates build
configuration from requests and execution context, consolidates shared
toolchain/JSON/runtime invocation services, and splits the worktree, bootstrap,
and documentation domains. Public compatibility removal is deliberately the
last implementation stage so structural changes and user-visible grammar
changes are not mixed.

The pre-change Python baseline passed 334 tests in 14.18 seconds using the
repository `.venv`. `test_command_contract.py` now freezes all 54 registered
command/group paths, a valid invocation for every leaf command, direct/shell
parse parity, group defaults, hidden compatibility inputs and warnings, global
aliases, help text digests, CLI exit codes, stream ownership, repository-root
forwarding, legacy root-scope restoration, and structured build-error fields.
The existing build-registry characterization now also freezes the exact shell
preset session keys and values. The completed Stage 0 suite passes 345 tests in
13.18 seconds, and all 145 active, completed, and archived plans validate.

### Stage 0 caller and compatibility inventory

| Input or boundary | Tracked callers and classification | Canonical replacement or removal gate |
| --- | --- | --- |
| `open-runtime` | No script or template caller. The registry declaration, compatibility tests, and `BuildAndRun.md`/`BuildSystem.md` deprecation text are the only tracked references. | `open runtime`; remove the complete alias slice in Stage 8 after the two lasting documentation references migrate. |
| Native-test `--target` and `--filter` | Live documentation examples, active-plan provenance, tests, registry/adapter fields, and runtime diagnostics still use them. | Positional `test <selection> [case-filter]`; migrate lasting documentation and diagnostics before Stage 8 removal. |
| `--granularity` / `hybrid` | Compatibility implementation, tests, and native-test/build documentation; no script or template caller. | `--mode isolation` for case isolation; target granularity is the ordinary default. Remove only after documentation and diagnostics migrate. |
| `--include-direct` | Accepted no-op with implementation, documentation, and tests; no operational caller. | No replacement is needed in target mode. Remove its registry, request, warning, runtime, documentation, and tests together in Stage 8. |
| `--ctest-regex` | Compatibility implementation, diagnostics, tests, and documentation; no script or template caller. | Positional case filter with `--mode isolation`; migrate recovery text before removal. |
| `--schedule-random` | Compatibility implementation, tests, and documentation; no script or template caller. | `--mode stress`. |
| `--output-junit` | Compatibility implementation, tests, and documentation; no script or template caller. | `--mode report [--report <path>]`. |
| Shell compact `build <target>`, `rebuild <target>`, and `run <args>` | Shell normalizer, characterization tests, and user-facing build/native-test documentation. No direct-command grammar or script/template caller. | Named direct forms remain `--target` and `--args` until the Stage 8 canonicalization decision. |
| Repository feature flags | One tracked repository config has every group enabled; schema, loader, registry filtering, and tests are the only other callers. No tracked disabled checkout configuration exists. | Confirm supported external checkout needs before removing the configuration surface in Stage 8. |
| `build.core` compatibility exports | No production module imports the facade. `test_build_core.py` is the only real test consumer; `test_build_config.py`, `test_build_output.py`, and `test_build_scaffolding.py` have unused imports. The consumed exports span toolchain, context/actions, runtime/tests, process, locking, recovery, and purge owners. | Move tests to their actual owner modules in Stage 7, delete the three unused imports, then remove re-exports with zero callers. |

## Goal

Make DurinDevTool a small command host over independently owned, explicitly
configured services. Each command domain must declare its grammar, construct a
typed request, execute through a bounded application service, and report
through explicit streams without depending on mutable module state or a
test-only compatibility facade.

The completed structure must have one owner for repository context, build
selection, toolchain capture, JSON structural validation, runtime program
invocation, Git worktree operations, directory-link transactions, and
documentation lifecycle operations. Compatibility inputs that no longer have
live callers must be removed only after canonical callers and documentation
have migrated.

## Scope

- Freeze direct-command and interactive-shell behavior before structural
  changes, including command paths, defaults, aliases, help, exit codes, and
  stdout/stderr ownership.
- Introduce immutable repository context and explicit command I/O values, then
  remove import-time repository discovery from feature services and temporary
  mutation of module-level repository roots.
- Move command specifications from the root registry into build, test, asset,
  scene, documentation, bootstrap, and worktree command modules while retaining
  one shared parser and help contract.
- Split build configuration models, configuration loading, profile/preset
  selection, typed command requests, build context, toolchain context, and
  action execution into separate owners.
- Consolidate Visual Studio/toolchain environment discovery and capture into
  one service shared by setup, preflight, and build execution.
- Extract the existing cached JSON Schema loading and diagnostic behavior into
  a shared JSON contract service used by repository config, user config,
  descriptors, and asset reports where schemas already exist.
- Share build selection, project resolution, executable location, process
  execution, cancellation, and failure diagnostics across runtime, scene, and
  asset-program invocation.
- Split worktree Git, directory-link, terminal, and transaction responsibilities
  without weakening removal safeguards or rollback behavior.
- Split third-party manifest decoding, source acquisition, installation,
  preflight, and setup responsibilities without changing dependency layout.
- Unify ordinary-document and lifecycle-document application ownership where
  doing so removes duplicate scans, parsing, or plan/roadmap dispatch.
- Move tests to the modules that own the tested behavior and remove test-only
  reliance on the build-core facade.
- Migrate and finally remove compatibility commands, aliases, options, and
  internal re-exports whose deletion gates are satisfied.

## Non-Goals

- Replacing `argparse`, the line-oriented interactive shell, or the existing
  batch entrypoint.
- Introducing a plugin discovery framework, dependency-injection container,
  event bus, service locator, or generalized workflow engine.
- Changing CMake presets, output layout, runtime variants, native-test target
  ownership, third-party source/install layout, or worktree safety policy.
- Rewriting the robust build child-process, lock, recovery, or crash-analysis
  implementations when ownership changes alone are sufficient.
- Changing native engine behavior, asset report formats, descriptor schemas,
  documentation formats, or archive semantics.
- Removing a public compatibility spelling before its repository callers,
  documentation, characterization tests, and stated removal window have been
  resolved.
- Splitting files solely to meet a line-count target. Every new boundary must
  remove mutable state, duplicated logic, invalid cross-domain access, or an
  unnecessary dispatch layer.

## Design Decisions and Invariants

### Repository context and command I/O are explicit

- A single immutable repository context owns the resolved checkout root and
  tracked repository configuration.
- Feature services accept the context or a narrower typed path value; they do
  not rediscover the repository at import time.
- No workflow temporarily overwrites a module-level repository root.
- Commands receive explicit stdout, stderr, and display policy. Handlers do not
  redirect process-global streams to adapt legacy functions.
- A dry run produces an explicit operation plan or passes an explicit dry-run
  policy; it never relies on swapping global output or root state.

### Command ownership remains declarative but moves to feature domains

- The root registry owns `CommandSpec`, aliases, capability filtering, parser
  composition, help, direct/shell dispatch, and deprecation emission.
- Each feature domain owns the arguments, defaults, summary, and handler for
  its command subtree.
- Leaf commands dispatch directly to a bounded adapter or use a typed dispatch
  table. A second long string-based action chain is not introduced behind the
  shared registry.
- Feature filtering is resolved before parser construction. The reusable
  parser is cached for the lifetime of a registry.
- Direct invocation and interactive-shell invocation parse the same canonical
  grammar. Any retained shell convenience is declared explicitly as
  compatibility behavior until Stage 8.

### Requests are typed by operation

- Build, test, run, purge, location, module creation, and project creation use
  separate immutable request types.
- A request does not expose unrelated action fields through empty-string,
  false, or default-enum fallback properties.
- CLI namespace adaptation happens at the command boundary. Application and
  domain services do not receive `argparse.Namespace`.
- Build context contains selected configuration and prepared execution state;
  it does not double as the command request or interactive-session store.
- Session-local preset selection uses a typed session value rather than an
  unstructured `dict[str, object]` contract.

### Shared services have one semantic owner

- Toolchain discovery and environment capture live outside build and bootstrap
  orchestration; callers may translate context but do not reimplement or wrap
  same-named primitives.
- JSON Schema owns structural fields, types, enums, required properties, and
  additional-property rejection. Python owns semantic rules such as repository
  containment, deterministic ordering, state transitions, and summary/count
  consistency.
- Runtime-program invocation shares build selection, project/executable
  resolution, cancellation, process diagnostics, and log reporting. Asset and
  scene domains retain their own arguments, report semantics, and rendering.
- Error subclasses preserve structured diagnostic fields and derive from the
  root DevTool error contract. Handlers do not discard structure by rethrowing
  only `str(exc)`.

### Splits follow transaction and ownership boundaries

- Worktree Git operations, directory-link primitives, terminal layout, and
  preparation/removal transactions are separate modules. The transaction
  service remains the only owner of detach/restore ordering and rollback.
- Bootstrap manifest decoding, source acquisition, installation, preflight,
  and checkout setup are separate modules. Source and install layout remain
  manifest-driven.
- Documentation discovery/reference/change operations, task lifecycle, and
  plan/roadmap lifecycle may use separate command adapters while sharing one
  application workspace and catalog where their data overlaps.
- Tests import the module that owns behavior. A facade retained only to keep
  old tests passing is not an architectural boundary.

### Compatibility changes happen last

- Stages 1 through 7 preserve the frozen canonical and compatibility grammar.
- Stage 8 removes only entries whose callers and documentation have migrated
  and whose characterization proves the canonical replacement.
- Each compatibility removal deletes the full vertical slice: registry input,
  namespace translation, request fields, validation branches, runtime branches,
  warnings, documentation, and obsolete tests.

## Current Foundations and Gaps

| Area | Current foundation | Gap to close |
| --- | --- | --- |
| Command model | One `CommandSpec` tree supports direct and shell parsing, nested help, aliases, feature filtering, and lazy handler import. | `registry.py` owns every domain's metadata and rebuilds the full parser for each shell command; several handlers dispatch a second time by string action. |
| Repository configuration | `RepositoryConfig` provides typed paths and feature values. | Build, bootstrap, worktree, and template modules discover and cache root/config at import time; legacy context managers temporarily mutate module globals. |
| Build services | Locations, output, process, locking, recovery, purge, runtime, tests, and scaffolding are separate modules. | `build/core.py` re-exports extracted services for compatibility, tests depend on the facade, and remaining context/toolchain/action concerns are still combined. |
| Build request model | Action-specific option dataclasses exist. | `CommandRequest` tags an option union with `Action`, then flattens it through many fallback properties and repeats action/type branching in construction and validation. |
| Toolchain | `toolchain.py` has shared environment parsing, VS discovery, and Windows capture. | Build core and bootstrap preflight retain same-named wrappers or duplicate environment access logic and error translation. |
| JSON validation | Descriptor loading has cached JSON Schema validation and useful JSON-path diagnostics; schemas exist for repository/user config and asset reports. | Repository config, user config, and asset reports repeat structural validation in Python; asset schemas are exercised only by tests. |
| Runtime invocation | Build process execution has logging, cancellation, timeouts, crash diagnostics, and output policy. | Scene and asset commands repeat profile/preset/project/executable selection, and asset uses a separate raw subprocess path. |
| Worktrees | Current workflow safely validates registration, cleanliness, expected links, detach order, and rollback. | One 771-line service owns Git, terminal layout, link primitives, preparation, removal, output, and mutable root state. |
| Bootstrap | Setup, preflight, dependency manifests, and agent config are already separate files. | Dependency preparation still combines manifest loading, source retrieval, verification, build/install, selection, and global repository scoping. |
| Documentation | `DocumentWorkspace`, generic catalog, task validation, lifecycle parsing, change sets, and archive transactions exist. | Ordinary-document and lifecycle handlers use parallel catalogs and long string dispatch; roadmap support is primarily thin plan wrappers. |
| Tests | Nearly 5,000 lines cover command, build, worktree, documentation, asset, and scene behavior. | Large test files mirror old facades and obscure actual module ownership, preventing removal of compatibility re-exports. |

## Implementation Stages

### Stage 0: Freeze command, compatibility, and dependency contracts

- [x] Add or update table-driven characterization for every top-level and
  nested command path, including defaults, aliases, hidden compatibility
  inputs, help, exit codes, and stdout/stderr ownership.
- [x] Freeze direct/shell parity for canonical commands and explicitly record
  shell-only compact normalization as compatibility behavior.
- [x] Search tracked source, scripts, templates, documentation, and active plans
  for `open-runtime`, legacy native-test options, build/run compact forms, and
  feature-flag assumptions.
- [x] Classify every compatibility input as live, documentation-only,
  test-only, or unreferenced, and record its canonical replacement.
- [x] Record the current DurinDevTool Python test result and command-help
  snapshots using the repository testing workflow.
- [x] Add focused characterization for repository roots, output streams, error
  metadata, and session preset state before changing those boundaries.
- [x] Confirm which `build.core` exports are used by production and which are
  retained only by tests.

#### Acceptance Gate

- The current interface and compatibility surface are represented by tests or
  an evidence-backed caller inventory.
- No structural or user-visible behavior has changed.
- Every later compatibility deletion has a named prerequisite and canonical
  migration path.

### Stage 1: Introduce explicit repository context, command I/O, and error ancestry

- [x] Add immutable `RepositoryContext` with the resolved repository root and
  tracked `RepositoryConfig`.
- [x] Construct the repository context once at the CLI boundary and pass it
  through registry execution.
- [x] Add a small explicit command-I/O value containing stdout, stderr, and
  plain/styling policy where needed.
- [x] Migrate build configuration paths from import-time globals to context or
  narrower typed path inputs.
- [x] Migrate worktree, bootstrap, preflight, agent-config, and scaffolding
  template code away from import-time root/config discovery.
- [x] Remove both legacy `repository_paths()` context managers after all
  consumers accept explicit context.
- [x] Make worktree and bootstrap services write through explicit command I/O;
  remove handler-level `redirect_stdout` and `redirect_stderr`.
- [x] Make domain errors derive from `DevToolError` while preserving build
  command, exit-code, recovery, excerpt, log, process, and timing fields.
- [x] Remove handler error conversions that rethrow only string messages.

#### Acceptance Gate

- No production feature service mutates a module-level repository root or
  redirects process-global output.
- Two repository contexts can be exercised in one process without state
  leakage.
- Existing command output, exit codes, and structured build diagnostics remain
  unchanged.
- Setup remains usable before the prepared Python environment exists.

### Stage 2: Move command specifications to feature-owned modules

- [x] Create command-spec modules for core, build/test, asset, scene,
  documentation, bootstrap/dependency, and worktree domains.
- [x] Move each domain's arguments, defaults, summaries, epilogs, feature, and
  handler registration out of the root registry.
- [x] Keep `CommandSpec`, `ArgumentSpec`, alias handling, capability checks,
  help, parser composition, and dispatch in the root command infrastructure.
- [x] Replace long handler action chains with direct leaf handlers or bounded
  typed dispatch tables without eagerly importing optional domains.
- [x] Cache the fully composed parser after feature filtering and reuse it for
  direct and interactive parsing.
- [x] Keep group defaults, nested help, case-insensitive matching,
  leading-slash compatibility, and deprecation warning frequency unchanged.
- [x] Add an ownership test that prevents feature modules from registering
  commands through side effects at import time.

#### Acceptance Gate

- Root `registry.py` contains command infrastructure and root composition, not
  full argument declarations for every feature.
- Direct invocation and the interactive shell use one reusable parser and
  produce the frozen parse results.
- Disabled command domains remain absent from help and do not import their
  heavy handlers.
- Command-specific and nested help remain equivalent to the Stage 0 snapshots.

### Stage 3: Separate build models, config I/O, selection, requests, and context

- [x] Move profile, preset, local-config, environment-provider, output-mode,
  and test-mode values into a build models module.
- [x] Move repository/user build config and profile/preset manifest decoding
  into config-I/O modules that accept explicit paths.
- [x] Move profile selection, preset selection, cache-variable decoding, and
  preset path expansion into a selection module.
- [x] Define separate immutable requests for configure, build, test, run,
  purge, location, module creation, and project creation.
- [x] Adapt argparse namespaces to concrete request types in feature command
  adapters; do not pass namespaces into build application services.
- [x] Delete action-tagged option-union validation and unrelated fallback
  properties from `CommandRequest` as consumers migrate.
- [x] Split build context creation/derivation from toolchain preparation and
  action execution.
- [x] Replace the shell's unstructured build session dictionary with a typed
  session value that owns the selected profile/preset state.
- [x] Express toolchain requirements through concrete request/capability facts
  instead of a central action set with test-operation exceptions.

#### Acceptance Gate

- Invalid request/option combinations are unrepresentable or rejected at the
  CLI adapter boundary.
- Application services receive concrete request types and never inspect
  unrelated fields through default values.
- Discovery commands do not initialize compiler environment, CMake, jobs, or
  operation locks.
- Build, test, run, path, status, purge, and scaffolding behavior remains
  equivalent to Stage 0.

### Stage 4: Consolidate toolchain, JSON contracts, and runtime invocation

- [x] Make the shared toolchain module the sole owner of environment-key
  casing, PATH lookup, VS discovery, setup-script capture, and environment
  output parsing.
- [x] Move Visual Studio environment-cache policy and compiler-diagnostic
  probing into a bounded build toolchain-context module.
- [x] Reduce bootstrap preflight to host prerequisite and minimum-version
  validation over the shared toolchain selection.
- [x] Remove same-named build/preflight wrappers after callers use the shared
  primitives and domain error ancestry directly.
- [x] Extract descriptor validation's cached schema loader, duplicate-key
  detection, JSON-path formatting, and first-error reporting into a shared
  JSON contract module.
- [x] Use the shared JSON contract for repository config, user config, and
  asset audit/migration reports whose checked-in schemas already define the
  structural contract.
- [x] Keep repository containment, deterministic report ordering, migration
  state transitions, and summary/count consistency as explicit semantic
  validators.
- [x] Add a runtime-program invocation service that consumes repository/build
  selection, project path, executable description, process policy, and command
  I/O.
- [x] Migrate scene and asset commands to the shared selection, project,
  executable, cancellation, logging, and process-failure behavior while
  retaining their domain-specific arguments and rendering.

#### Acceptance Gate

- Toolchain discovery and environment capture each have one implementation.
- Checked-in repository, user, descriptor, and asset schemas are exercised by
  production decoding rather than only tests.
- Structural schema rules are not duplicated as manual enum/required-field
  checks; semantic invariants remain explicitly tested.
- Scene, asset, and ordinary runtime program failures expose consistent exit,
  cancellation, excerpt, and log diagnostics.

### Stage 5: Split worktree and bootstrap by functional ownership

Stage 5 is complete. Worktree commands now route through explicit model, Git,
link, terminal, transaction, application-service, and command-adapter owners.
Bootstrap dependency preparation routes through separate manifest, source
acquisition, installer, preflight, setup, and application-service owners.
Both preparation domains expose immutable planned-operation values before
mutation, while worktree transactions retain ordered detach/rollback and
restoration-error aggregation.

- [x] Create worktree model, Git, link, terminal, transaction/service, and
  command-adapter modules.
- [x] Move worktree porcelain parsing, safe-directory arguments, add/remove,
  clean-state checks, and Git error handling into the Git owner.
- [x] Move symlink/junction inspection, containment, creation, detach, and
  restoration into the link owner.
- [x] Replace agent/VS Code link wrapper functions with data-driven shared
  directory specifications where their only difference is path and label.
- [x] Keep preparation/removal ordering, unexpected-link rejection, detach
  rollback, and restoration failure aggregation in the transaction service.
- [x] Move Windows Terminal pane layout and launching into the terminal owner.
- [x] Create bootstrap manifest, source-acquisition, installer, preflight,
  setup, and application-service modules.
- [x] Move manifest selection and validation away from archive/Git acquisition
  and CMake install execution.
- [x] Represent dependency preparation and worktree preparation dry runs as
  explicit planned operations where practical.

#### Acceptance Gate

- Worktree Git, links, terminal layout, and transaction behavior can be tested
  without importing one monolithic service module.
- Worktree remove retains clean-tree enforcement, expected-link validation,
  main-worktree protection, lock protection, rollback, and dry-run safety.
- Bootstrap manifests, source preparation, and installation can be tested as
  separate units with explicit repository context.
- Dependency locations and produced artifacts remain unchanged.

### Stage 6: Unify documentation application and lifecycle ownership

Stage 6 is complete. A single lifecycle configuration and workspace now owns
plan/roadmap titles, labels, directories, exclusions, filtering, rendering,
and archive policy. The unified documentation workspace caches ordinary,
task, and lifecycle catalogs, and production dispatch uses separate ordinary
document, task, and lifecycle command adapters with one parameterized archive
path. Full validation covers 352 Python tests and 246 documentation files.

- [x] Define one lifecycle configuration/model for plan and roadmap title
  suffix, label, directory, exclusions, archive policy, filtering, and
  rendering.
- [x] Replace roadmap's thin plan aliases with lifecycle configuration or a
  deliberately small compatibility-free adapter.
- [x] Route ordinary documentation, task, plan, and roadmap application
  operations through `DocumentWorkspace` or a renamed unified workspace.
- [x] Reuse one catalog scan when a command needs both generic document links
  and task/plan/roadmap metadata.
- [x] Split documentation command adapters into ordinary document, task, and
  lifecycle modules while retaining shared rendering and change transactions.
- [x] Remove duplicate plan/roadmap list, validate, and archive action branches
  in the unified handler.
- [x] Preserve changed-scope validation, archive reference repair, atomic
  changes, task domain rules, and bounded archive listing policy.

#### Acceptance Gate

- Plan and roadmap lifecycle behavior is parameterized rather than duplicated
  through parallel wrapper functions.
- Generic validation does not reload the same task or lifecycle domain without
  need.
- Documentation list/find/refs/validate/create/move/task/plan/roadmap outputs
  and archive transactions satisfy their existing tests.
- All-document and all-plan validation pass.

### Stage 7: Realign tests with module ownership and remove internal facades

Stage 7 is complete. Tests now patch the modules that own context, runtime,
process, locking, recovery, purge, bootstrap, Git, link, terminal, and worktree
transaction behavior. The test-only legacy request compatibility module and
the production-dead bootstrap/worktree facades have been removed. Architecture
tests enforce the principal command-adapter, application-service, model, and
infrastructure dependency directions, and lasting build-system documentation
records the final extension points. The complete suite passes 356 tests;
documentation and plan validation pass across active and archived scopes.

- [x] Split `test_build_core.py` into context, toolchain context, actions,
  runtime, process, locking, recovery, and purge test modules.
- [x] Split combined bootstrap/worktree tests into bootstrap setup, dependency
  manifest, worktree Git, worktree links, and worktree transaction tests.
- [x] Update tests to import actual owner modules instead of patched symbols
  re-exported by `build.core`.
- [x] Remove dead helpers, unused imports, same-name forwarding functions, and
  test-only facade exports whose production callers are gone.
- [x] Keep `build.core` only if it owns a coherent remaining application
  service; otherwise delete it after consumers migrate.
- [x] Add lightweight dependency-direction checks for command adapters,
  application services, domain models, and infrastructure modules.
- [x] Update lasting build/tooling documentation for the final module
  ownership and extension points rather than leaving those rules only here.
- [x] Run the complete DurinDevTool Python suite and scoped documentation
  validation using the repository agent workflows.

#### Acceptance Gate

- Tests import and patch the modules that own behavior.
- No compatibility facade exists solely to satisfy legacy test imports.
- Every retained module has one describable responsibility and at least one
  production consumer unless it is an entrypoint or model module.
- Full DurinDevTool Python and documentation validation pass before public
  compatibility removal begins.

### Stage 8: Remove expired compatibility and start bounded removal windows

Stage 8 is complete. The dead shell-help path, orphaned asset-audit v1
artifacts, `open-runtime`, `--include-direct`, shell-private build/run compact
normalization, repository feature flags, and `build.core` compatibility
re-exports have been removed. Native-test examples use positional selection
and focused filters. Command specifications now declare concrete Python module
requirements, so standard-library and discovery commands no longer inherit a
coarse prepared-environment gate. Retained native-test compatibility options
remain bounded by the completed selection-and-ownership contract. The final
suite passes 347 tests; all 246 documentation files and all active, completed,
and archived plans validate.

- [x] Remove the uncalled `print_shell_help` helper and its undefined
  `shell_command_help` reference if not already removed as dead code.
- [x] Confirm asset audit v1 fixtures/schema have no external consumer, then
  delete the orphaned v1 schema and fixture while retaining current report
  compatibility requirements.
- [x] Remove `build.core` service re-exports after Stage 7 proves there are no
  production or test callers.
- [x] Migrate the final documentation references to `open runtime`, remove the
  `open-runtime` alias and warning, and delete its compatibility tests.
- [x] Migrate repository documentation and scripts from native-test
  compatibility flags to positional selection, `--mode`, `--report`, and
  focused filters.
- [x] Remove deprecated native-test inputs one vertical slice at a time,
  beginning with the `--include-direct` no-op, only after the caller inventory
  reaches zero.
- [x] Decide whether compact shell-only `build <target>` and `run <args>` forms
  become canonical shared grammar or enter a warning period followed by
  removal; eliminate shell-private normalization after the selected path is
  complete.
- [x] Confirm whether any supported checkout intentionally disables DevTool
  feature groups. If none does, remove repository feature flags, schema fields,
  registry filtering, and feature-only tests.
- [x] Replace coarse `PREPARED_ENVIRONMENT` capability on pure discovery or
  standard-library commands with actual dependency requirements, then allow
  commands with no prepared dependency to run in the bootstrap interpreter.
- [x] Update command help, owning build/tooling documentation, templates, and
  tests with each compatibility removal.

#### Acceptance Gate

- Every removed compatibility input has zero tracked callers, a documented
  canonical replacement, and passing direct/shell tests.
- No registry argument, namespace field, request field, validation branch,
  runtime branch, warning, documentation example, or test remains for a
  physically removed input.
- Retained compatibility entries have a named caller, explicit removal gate,
  and bounded review date or milestone.
- Canonical build, test, run, location, asset, scene, documentation, bootstrap,
  and worktree workflows pass the complete validation matrix.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Command contract | Table-driven direct/shell parsing, nested help, defaults, invalid options, warnings, stdout/stderr, and exit codes |
| Repository context | Two-root isolation tests; no import-time root mutation; explicit path injection across build, bootstrap, worktree, asset, and scene |
| Command registry | Domain-owned specs compose one cached parser and handlers stay lazily imported |
| Typed requests | Every command adapter constructs the correct concrete request; invalid cross-action fields are unrepresentable or rejected at the boundary |
| Build selection/context | Profile/preset/session selection parity; discovery remains toolchain- and lock-free; prepared commands retain environment behavior |
| Toolchain | VS discovery, setup-script capture, environment casing, cache invalidation, minimum versions, Ninja discovery, and compiler diagnostic language |
| JSON contracts | Duplicate keys, malformed JSON, schema paths, unknown fields, enums, required values, containment, ordering, and cross-field semantics |
| Runtime invocation | Executable/project resolution, arguments with spaces, cancellation, timeout, logs, excerpts, exit codes, crash diagnostics, and missing artifacts |
| Worktrees | Porcelain parsing, safe-directory policy, main/locked/dirty protection, expected/unexpected links, dry run, detach, rollback, and terminal layout |
| Bootstrap | Manifest validation/selection, Git/archive verification, hashing, extraction, CMake install, preflight, venv, and linked-worktree setup |
| Documentation | Catalog/search/reference validation, changed scope, tasks, lifecycle metadata, archive preview/apply, reference repair, and atomic rollback |
| Compatibility | Caller inventory, canonical migration, warning period where required, complete vertical deletion, and final repository search |
| Complete regression | Full DurinDevTool Python suite and repository documentation validation; native validation only when changed process/build/runtime behavior requires it under the agent workflows |

## Definition of Done

- Repository root and tracked DevTool configuration are constructed once and
  passed explicitly; feature services do not mutate or rediscover global root
  state.
- Handlers do not redirect process-global stdout/stderr or discard structured
  domain errors through string-only translation.
- Root command infrastructure composes feature-owned specifications and reuses
  one parser for direct and interactive commands.
- Build configuration, selection, typed requests, context, toolchain context,
  and action execution have separate owners.
- Application services do not receive `argparse.Namespace` or access unrelated
  request fields through fallback properties.
- Toolchain discovery/capture, JSON structural validation, and runtime-program
  invocation each have one semantic implementation.
- Worktree and bootstrap modules follow Git/link/terminal/transaction and
  manifest/source/install/preflight/setup ownership respectively.
- Documentation plan and roadmap lifecycle behavior shares one parameterized
  owner without competing catalogs or duplicate handler branches.
- Tests target actual owner modules, and no build-core compatibility facade is
  retained solely for tests.
- Compatibility inputs with no callers are removed completely; retained inputs
  have explicit removal gates.
- Lasting architecture and user-facing behavior are documented in the owning
  tooling/build documents, and the complete required validation passes.

## Deferred Follow-ups

- Shell completion, persistent history, a full-screen terminal UI, and
  background command execution.
- Third-party command/plugin discovery for external projects; feature-owned
  in-repository spec modules are sufficient for this plan.
- A general workflow engine for build, bootstrap, worktree, or documentation
  transactions.
- Machine-readable output for commands that do not already expose a stable
  structured format.
- Cross-process daemonization or concurrent command execution; this plan makes
  services reentrant but does not add a daemon.
- Replacing Python or `argparse`, or packaging DurinDevTool as a separately
  distributed product.

## Related Documentation

- `Documentation/README.md`
- `Documentation/Agents/BuildAndRun.md`
- `Documentation/Agents/Testing.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/NativeTests.md`

## Related Code

- `DevTool.bat`
- `Tools/DurinDevTool/durin_dev_tool/registry.py`
- `Tools/DurinDevTool/durin_dev_tool/shell.py`
- `Tools/DurinDevTool/durin_dev_tool/configuration.py`
- `Tools/DurinDevTool/durin_dev_tool/toolchain.py`
- `Tools/DurinDevTool/durin_dev_tool/build/config.py`
- `Tools/DurinDevTool/durin_dev_tool/build/core.py`
- `Tools/DurinDevTool/durin_dev_tool/build/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/build/operations.py`
- `Tools/DurinDevTool/durin_dev_tool/build/process.py`
- `Tools/DurinDevTool/durin_dev_tool/build/runtime.py`
- `Tools/DurinDevTool/durin_dev_tool/build/descriptors.py`
- `Tools/DurinDevTool/durin_dev_tool/worktree/services.py`
- `Tools/DurinDevTool/durin_dev_tool/worktree/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/dependencies.py`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/preflight.py`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/setup.py`
- `Tools/DurinDevTool/durin_dev_tool/asset.py`
- `Tools/DurinDevTool/durin_dev_tool/scene.py`
- `Tools/DurinDevTool/durin_dev_tool/documentation/service.py`
- `Tools/DurinDevTool/durin_dev_tool/documentation/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/documentation/plans.py`
- `Tools/DurinDevTool/durin_dev_tool/documentation/roadmaps.py`
- `Tools/DurinDevTool/tests/test_build_core.py`
- `Tools/DurinDevTool/tests/test_build_registry.py`
- `Tools/DurinDevTool/tests/test_bootstrap_worktree.py`
- `Tools/DurinDevTool/tests/test_documentation_domain.py`
- `Tools/DurinDevTool/tests/test_asset_audit.py`
