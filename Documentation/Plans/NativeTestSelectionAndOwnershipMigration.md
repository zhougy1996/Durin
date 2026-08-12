# Native Test Selection and Ownership Migration Plan

Summary: Add structured native-test metadata and focused set selection, then migrate representative feature, contract, and integration targets without requiring a repository-wide test reorganization.

Last reviewed: 2026-08-12

Status: Active
Completed:

## Current Status

Durin's native-test infrastructure already has strong execution foundations:
each source and suite has one target owner, targets run through the common
harness, runtime dependency closure is derived from real link edges, CTest
registrations carry labels and resource locks, target-level execution is the
default, and the repository guidance already says that test targets should own
coherent feature or lifecycle slices rather than mirror production modules.

The remaining problem is not a missing test framework. It is that selection and
declaration have not caught up with the growing feature set:

- `DevTool test` focuses one named target or runs the complete aggregate, but
  has no first-class way to select a bounded feature, contract, backend, or
  runtime-stack set.
- Existing labels are mostly execution-policy labels and free-form additions;
  they do not form a validated taxonomy that tooling can safely query.
- `EngineTests/CMakeLists.txt` declares many unrelated feature and integration
  targets through one broad helper with an implicit Engine-heavy dependency
  baseline.
- Several targets compile production `Private/*.cpp` files from other modules
  directly, which can bypass the link/export boundary that the feature is
  supposed to validate.
- Directory names still suggest module ownership even when the contained
  targets correctly exercise cross-module features.

The selected migration is deliberately incremental. First extend CMake with a
compatible structured declaration and generated registry. Then teach
DurinDevTool to build and run registry-selected target sets while preserving
CTest scheduling and locks. Migrate a small group of representative targets to
prove the model. After that, require the new declaration for new targets and
migrate legacy targets when their feature area is substantively changed.

No implementation stage has started. Existing targets, commands, names, and
directories remain authoritative until their stage acceptance gate passes.

## Goal

Make focused validation the normal and reliable development workflow as the
repository grows, without forcing a one-time migration of every native test.

After the plan:

- Every newly created native-test target declares a validated test kind and one
  or more stable selection domains.
- DurinDevTool can list, explain, build, and run a bounded set selected from the
  generated CMake registry without first building `DurinNativeTests`.
- Routine commands use one positional selection plus an optional case filter;
  isolation, stress scheduling, and report generation are exposed as named
  modes rather than a flat collection of interacting low-level flags.
- Module contract tests, cross-module feature tests, backend/system integration
  tests, characterization tests, and infrastructure tests remain distinct but
  use the same harness and scheduling machinery.
- Test target ownership means ownership of a coherent setup and behavior
  contract, not mandatory alignment with one production module.
- Migrated cross-module feature targets link real production modules rather
  than compiling foreign production-private sources into the test executable.
- Existing target names and focused `test --target <name>` commands continue to
  work throughout the compatibility period.
- `test --target all` remains available as a low-frequency aggregate gate, not
  a routine prerequisite for ordinary implementation changes.

## Scope

- Define a small, validated native-test metadata taxonomy in CMake.
- Add one generic structured target-declaration path that composes the existing
  `add_durin_test`, execution, discovery, deployment, lock, and timeout helpers.
- Generate a machine-readable configured-test registry from final target
  metadata.
- Add DurinDevTool commands for listing/explaining test targets and selecting a
  bounded set from the registry.
- Simplify the `test` command around routine, diagnostic, stress, and CI/report
  scenarios while retaining bounded compatibility for existing scripts.
- Preserve CTest resource locks, timeouts, target-level execution, shuffle,
  diagnostics, and JUnit behavior for selected sets.
- Add repository-policy checks for missing/invalid new metadata and forbidden
  foreign production-private source compilation in migrated targets.
- Establish compatibility rules for unmodified legacy declarations.
- Migrate representative World, Launch, and Viewport test slices.
- Split or add module-owned contract tests where the representative migrations
  expose a genuine narrow module contract.
- Update native-test authoring and validation documentation.
- Define routine, risk-expanded, aggregate, nightly, and release validation
  policy without requiring a specific hosted CI provider.

## Non-Goals

- Moving every test source into a new directory hierarchy in one change.
- Renaming every existing target or changing suite names solely for taxonomy.
- Replacing GoogleTest, CTest, NativeTestSupport, DurinDevTool, or the existing
  runtime-deployment closure.
- Automatically proving semantic impact from arbitrary C++ source changes in
  the first implementation.
- Making module boundaries the sole or primary test-organization dimension.
- Prohibiting all white-box tests. Same-owner private implementation testing
  remains possible through an explicit module-owned seam.
- Eliminating `test --target all`, case-granularity diagnosis, randomized
  aggregate qualification, GPU locks, or characterization targets.
- Requiring every pull request, local implementation, or agent handoff to run
  the complete native aggregate.
- Creating a second source-ownership validator or duplicating link/runtime
  metadata in a handwritten test manifest.
- Mixing unrelated production-module refactors into the CMake/tooling stages.
- Removing isolation, randomized scheduling, JUnit output, timeout override, or
  case filtering merely to make the help text shorter.
- Treating physical directory location as authoritative once structured target
  metadata is available.

## Design Decisions and Invariants

### Test ownership follows the verified contract

- A target owns suites that share a coherent setup, dependency stack, resource
  policy, process lifetime, and failure domain.
- A module-contract target verifies one module's public or explicitly owned
  test seam. It normally links only that module and lower-level dependencies.
- A feature target may intentionally link multiple production modules when the
  user-visible or runtime behavior crosses those boundaries.
- An integration target verifies a concrete backend, complete runtime stack,
  process boundary, editor startup, cooking path, or other system assembly.
- A characterization target owns intentional crashes, abrupt exits, hostile
  state, or behavior that cannot enter the ordinary aggregate.
- An infrastructure target verifies the test/build tooling itself.
- No rule requires a target's directory name to equal a production module, and
  no module label implies exclusive ownership of a cross-module feature.

### Metadata is small, structured, and selection-oriented

- Each migrated or new target declares exactly one `KIND` from a closed set:
  `contract`, `feature`, `integration`, `characterization`, or
  `infrastructure`.
- Each migrated or new ordinary target declares one or more `DOMAINS` such as
  `world`, `viewport`, `asset-package`, `texture`, `renderer`, `launch`, or
  `editor-shell`. Domains describe stable validation slices rather than source
  directories or team names.
- Optional `MODULES` describe production-module contracts or principal module
  participants. They aid discovery and impact expansion but never redefine
  CMake link dependencies.
- Optional `BACKENDS` describe a real backend requirement such as `vulkan`.
  Backend metadata does not replace runtime-only dependency declarations or
  resource locks.
- Optional `STACKS` describe expensive runtime assembly such as `editor`,
  `renderer`, or `process`. Stack metadata does not replace the existing heavy
  runtime rationale.
- Existing timeout, case-parallel safety, resource locks, lifecycle,
  editor-only availability, runtime-only dependencies, and checked-in data
  declarations remain orthogonal authoritative properties.
- CMake emits normalized labels from structured values, for example
  `kind-feature`, `domain-viewport`, `module-engine`, `backend-vulkan`, and
  `stack-editor`. Callers do not handcraft reserved-prefix labels.
- Stage 0 freezes the exact keywords and selector syntax before implementation;
  the conceptual dimensions above are selected, while spelling may be refined
  once to fit the existing command interface.

### CMake remains the source of truth

- Target metadata is declared beside the target after its complete source/link
  ownership is known and before `durin_discover_tests` finalizes it.
- The registry is generated from configured CMake targets and their properties;
  no manually synchronized JSON/YAML catalog is checked in.
- The generated registry contains at least target name, availability, kind,
  domains, modules, backends, stacks, direct-lifecycle status, timeout,
  resource locks, and heavy-runtime classification.
- The registry excludes targets unavailable in the selected preset but records
  configured exclusions when the existing exclusion mechanism provides enough
  information to explain them.
- DurinDevTool rejects a stale registry using the same preset/build identity
  rules already used by build and test operations rather than silently running
  an outdated selection.
- The existing focused target command remains the most direct single-target
  path and does not require registry selection.

### Compatibility prevents a flag-day migration

- Existing unmodified declarations continue to configure and run through their
  current helpers during the migration period.
- The new structured helper is mandatory for newly added targets after its
  foundation gate passes.
- A legacy target becomes migration-required when work adds a new suite,
  materially changes its dependency/runtime stack, or splits/merges its
  lifecycle boundary. A small assertion correction does not force unrelated
  CMake movement.
- Migrated targets cannot return to the legacy declaration path.
- Compatibility metadata is visible in the generated registry and in list
  output; it is not represented as a permanent test kind.
- No acceptance gate depends on migrating every legacy target. The plan can
  complete after the infrastructure, representative migrations, enforcement
  for new/substantively changed targets, and lasting migration guidance are
  established.

### Selected-set execution preserves real scheduling

- Selection resolves to an explicit target list before build starts and prints
  both the predicate and resolved targets.
- DurinDevTool builds only the selected test executables plus their ordinary
  CMake dependency/runtime deployment closure. It does not build
  `DurinNativeTests` merely to run a subset.
- Multi-target execution uses the existing CTest direct target registrations
  so timeout, resource locks, target-level lifecycle, working directories,
  shuffle policy, and result reporting remain authoritative.
- An empty selector is an error with an explanation; it never falls back to
  `all`.
- A selector that includes characterization tests requires an explicit
  characterization mode and never silently admits them into ordinary sets.
- Selection supports union within one dimension and intersection across
  dimensions. Stage 0 freezes exact command grammar and escaping with tests.
- Every selected-set run reports the resolved target list and enough predicate
  information to reproduce it with named focused commands.

### DurinDevTool presents scenarios, not CTest implementation details

- The ordinary command shape is positional and identical in batch and
  interactive use. The target experience is conceptually:

  ```text
  test CoreUtilityTests
  test CoreUtilityTests FJsonDocumentTests.ParseObjectFromString
  test @viewport
  test all
  ```

  Stage 0 freezes the exact selected-set marker and compatibility spelling, but
  ordinary users do not need to write `--target` for the common path.
- Exact target names win over shorthand set names. Set syntax has an explicit
  marker or keyword so adding a target cannot silently change an existing
  command's meaning.
- `test list [query]` shows concise target/set choices for the active preset.
  `test explain <selection>` shows resolved targets, kind/domain/backend/stack,
  heavy resources, and why they match without building them.
- Routine execution defaults to one process per selected target and the normal
  timeout. Users do not select target/case/hybrid granularity for ordinary
  work.
- Case isolation is a diagnostic scenario with a mandatory narrow case
  selector. Randomized scheduling is a stress scenario. JUnit emission is a
  report/CI scenario with a predictable default output path and an optional
  override. These capabilities may be expressed as subcommands or one
  validated `--mode`; Stage 0 selects whichever integrates most cleanly with
  the command registry.
- `--filter` remains the one user-facing case-pattern concept. DurinDevTool
  translates it to a GoogleTest filter for one target or to the appropriate
  CTest discovered-case selection only in explicit isolation mode. Users are
  not normally asked to choose between `--filter` and `--ctest-regex`.
- `hybrid` granularity and `--include-direct` are hidden compatibility inputs
  during a documented deprecation window because target execution has already
  replaced hybrid behavior and include-direct is a no-op. They do not appear in
  routine help or new examples.
- `--schedule-random`, `--output-junit`, `--ctest-regex`, and explicit
  `--granularity` remain accepted for existing automation during the first
  compatibility stage, but help points new callers to the scenario-oriented
  spelling.
- Invalid combinations fail before configure/build and report a concise reason
  plus one copyable corrected command. The tool never requires a user to infer
  which low-level parameter unlocks another parameter.
- `test --help` leads with four common examples and separates routine options
  from advanced compatibility options. Interactive completion/listing uses the
  generated registry instead of requiring users to remember target names.

### Full build and full test have separate purposes

- A full `all` build verifies repository-wide compilation/link integration and
  remains required by existing user-visible editor handoff rules.
- `test --target all` verifies every ordinary native target and is reserved for
  explicit plan/release gates, shared test/runtime infrastructure changes,
  changes crossing test targets whose impact cannot be bounded reliably,
  evidence of process-global leakage/order dependence, and scheduled CI.
- Ordinary production changes run the smallest directly affected targets plus
  risk-expanded feature/integration sets selected by metadata.
- A target migration that changes only declaration/location and preserves its
  link/runtime behavior validates the migrated target and relevant CMake policy
  probes; it does not require the complete native aggregate by default.
- CMake registry, discovery, harness, aggregate, selection, locking, or shared
  deployment changes do require the full native aggregate because they alter
  shared test infrastructure.
- Nightly/release aggregate policy is not evidence that every local change must
  pay the same cost.

### Private implementation tests use explicit owned seams

- A migrated feature or integration target may not compile a production
  `Private/*.cpp` owned by another module directly into its executable.
- When a pure unit needs private implementation access, the owning module may
  expose a dedicated test-support target or an internal library/object boundary
  with explicit ownership and no runtime export promise.
- When production logic is reusable across the feature and tests, prefer a
  normal cohesive production component linked by both rather than duplicating
  its `.cpp` compilation in multiple targets.
- Test-only access headers remain narrow and must not become a substitute for a
  missing production feature interface.
- Same-owner white-box compilation exceptions, if retained, carry an explicit
  property and rationale so the repository validator can distinguish them from
  accidental foreign-source inclusion.
- Migration is allowed to expose a small public contract when that contract is
  genuinely useful to production consumers; it must not export internals only
  to make a test migration mechanically easy.

## Current Foundations and Gaps

| Area | Existing foundation | Migration gap |
| --- | --- | --- |
| Target execution | Focused `test --target`, target-level aggregate registration, filtering, timeout, shuffle, and JUnit support are established. | No bounded multi-target selection other than the complete aggregate. |
| Discovery policy | `DURIN_TEST_LABELS`, resource locks, lifecycle policy, and generated CTest property files already exist. | Labels are not validated into stable kind/domain/module/backend/stack dimensions. |
| Source ownership | Configuration rejects unowned and duplicate native-test sources and suites. | It does not express semantic target classification or consistently reject foreign production-private source compilation. |
| Runtime closure | Runtime DLL/file deployment derives from real target linkage with explicit runtime-only exceptions. | The broad Engine test helper adds a large common link/include baseline that obscures the minimum feature stack. |
| Target organization | Many targets already have feature-oriented names such as `TextureTests`, `ViewportTests`, and `LaunchProcessBoundaryTests`. | Declarations are concentrated in module-looking directories/CMake files and lack queryable ownership metadata. |
| Aggregate | `DurinNativeTests` and CTest default labels provide a qualified full-suite path. | Routine workflow still has a practical one-target-or-everything selection gap. |
| Command UX | The interactive shell accepts compact `test <target> [filter]` forms and the batch command exposes every underlying capability. | Batch and shell forms differ; routine help mixes target, case isolation, hybrid compatibility, randomization, regex, and report flags with several conditional combinations. |
| Documentation | Native-test guidance already prefers feature/lifecycle targets and focused runs. | It does not define the structured taxonomy, compatibility migration, selected-set workflow, or low-frequency aggregate policy. |

## Implementation Stages

### Stage 0: Freeze taxonomy, selector semantics, and baseline

- [ ] Inventory configured ordinary and characterization targets from CMake
  rather than recording a long-lived target count in documentation.
- [ ] Classify the representative World, Launch, and Viewport targets using the
  proposed kind/domain/module/backend/stack dimensions and remove any dimension
  that does not improve an actual selection decision.
- [ ] Freeze reserved label prefixes, allowed value syntax, normalization,
  duplicate handling, and validation errors.
- [ ] Select and document the DurinDevTool grammar for list, explain, and
  selected-set execution, including union/intersection, empty results,
  unavailable preset targets, and characterization admission.
- [ ] Freeze one consistent positional command grammar for batch and
  interactive use, including exact target, explicit set, `all`, and optional
  single-target case filter forms.
- [ ] Select the scenario-oriented surface for routine, isolation, randomized
  stress, and JUnit/report execution; map every current test flag to its new
  owner or compatibility-only status.
- [ ] Define the deprecation window and warning policy for `--target`,
  `--granularity hybrid`, `--include-direct`, `--ctest-regex`,
  `--schedule-random`, and `--output-junit` without breaking existing CI in the
  first stage.
- [ ] Prototype help/list/explain output and verify that the four common tasks
  require no knowledge of CTest labels or execution granularity.
- [ ] Freeze the generated registry schema, preset/build identity, location,
  and stale-registry failure behavior.
- [ ] Record representative focused and aggregate configure/build/test timings
  as migration evidence without turning wall-clock values into correctness
  gates.
- [ ] Inventory foreign production-private source compilation and distinguish
  same-owner white-box seams from cross-owner feature-test shortcuts.

#### Acceptance Gate

- The metadata vocabulary, registry record, selector behavior, compatibility
  boundary, private-source policy, and validation-cost model are unambiguous;
  each proposed field supports at least one concrete selection or enforcement
  use case.

### Stage 1: Add compatible structured CMake declarations

- [ ] Add a generic structured native-test declaration/finalization path that
  composes the existing lower-level helpers rather than duplicating discovery,
  deployment, timeout, locks, or lifecycle logic.
- [ ] Store validated kind, domains, modules, backends, and stacks as target
  properties and emit normalized reserved labels into existing CTest
  registrations.
- [ ] Generate the configured native-test registry from final target
  properties.
- [ ] Mark legacy targets explicitly in the generated registry while allowing
  their current helpers to continue unchanged.
- [ ] Add positive and negative CMake policy probes for valid metadata, missing
  kind/domain, invalid reserved labels, characterization misuse, duplicates,
  unavailable configurations, and registry determinism.
- [ ] Add enforcement that new structured declarations cannot include foreign
  production-private sources without an owned seam; keep legacy declarations
  compatible until migrated.
- [ ] Document the structured CMake authoring pattern and compatibility rule.
- [ ] Because this stage changes shared native-test infrastructure, run the
  policy/runtime-closure probes and the complete native aggregate at default
  target granularity.

#### Acceptance Gate

- New and legacy targets coexist in one configure; structured metadata reaches
  CTest and a deterministic generated registry; invalid declarations fail with
  actionable diagnostics; all existing ordinary tests retain their execution,
  lock, timeout, deployment, and aggregate behavior.

### Stage 2: Simplify DurinDevTool and add bounded test-set selection

- [ ] Make positional `test <selection> [case-filter]` work consistently from
  `DevTool.bat` and the interactive shell while retaining `--target` as a
  compatibility alias.
- [ ] Add commands to list available target metadata and explain why a target
  matches a selector in the active preset.
- [ ] Resolve selected sets from the generated registry before invoking CMake
  and print the exact target list.
- [ ] Build only resolved target executables and their dependency closures.
- [ ] Execute selected targets through CTest direct registrations while
  preserving resource locks, timeouts, target-level process lifetime, shuffle,
  JUnit, failure output, cancellation, and recovery semantics.
- [ ] Reject stale registry, empty selection, incompatible focused filters, and
  implicit characterization inclusion.
- [ ] Implement the selected routine/isolation/stress/report scenarios with
  safe defaults and move low-level compatibility flags out of routine help.
- [ ] Give JUnit/report mode a deterministic default artifact path while
  retaining an explicit path override for automation.
- [ ] Unify ordinary case-pattern terminology under `filter`; retain raw CTest
  regex only as an advanced compatibility escape hatch where exact discovered
  registration matching is genuinely required.
- [ ] Emit one copyable recovery command for unsupported combinations and one
  short selection summary before a multi-target build begins.
- [ ] Add registry-backed interactive completion or an equivalent discoverable
  target/set listing so users do not need to memorize names.
- [ ] Add DurinDevTool unit tests for grammar, selection algebra, escaping,
  preset availability, build command composition, CTest command composition,
  compatibility aliases, help grouping, recovery suggestions, error behavior,
  and result reporting.
- [ ] Preserve `test --target <name>` and `test --target all` without semantic
  changes.
- [ ] Because this stage changes shared test execution tooling, run its Python
  tests, the native policy probes, representative locked/GPU selections, and
  the complete native aggregate.

#### Acceptance Gate

- A developer can run one target, one case, one bounded feature set, or all
  ordinary tests using a short consistent command; advanced isolation, stress,
  and report scenarios remain available without exposing their low-level CTest
  mechanics in the routine path.

### Stage 3: Migrate World as the low-dependency feature pilot

- [ ] Re-declare `WorldTests` with structured `feature/world` metadata and its
  actual minimum module/stack participation.
- [ ] Separate a narrow Engine World contract target only if suites demonstrably
  use a smaller lifecycle and dependency stack; do not split merely to mirror
  the Engine module.
- [ ] Remove broad helper-injected libraries/includes not used by the migrated
  target and preserve suite/source single ownership.
- [ ] Verify lifecycle reset, sandbox behavior, direct target registration, and
  selected-set discovery.
- [ ] Run the migrated World target, its selected domain set, CMake policy
  probes, and a full build only if production public/export boundaries change.

#### Acceptance Gate

- The World slice proves that a feature target can migrate without directory or
  suite renaming, has a minimal declared stack, remains directly runnable, and
  is discoverable through the structured selector.

### Stage 4: Migrate Launch as the private-seam pilot

- [ ] Classify Launch argument/storage contract targets and process-boundary or
  crash targets into their correct contract, integration, and characterization
  kinds.
- [ ] Stop representative Launch feature targets from compiling Launch-private
  production `.cpp` files through the EngineTests broad helper.
- [ ] Introduce the smallest Launch-owned internal/test-support target or normal
  production component needed to share pure grammar/storage logic without
  exporting Launch internals indiscriminately.
- [ ] Preserve process isolation, crash artifacts, timeouts, runtime child
  dependencies, and aggregate exclusion for characterization.
- [ ] Validate focused Launch contracts, the process-boundary set, explicit
  characterization execution, source ownership, and policy probes.
- [ ] Do not run the complete aggregate solely because target declarations
  moved; require it only if shared harness/registration policy changes or
  evidence shows cross-target leakage.

#### Acceptance Gate

- Launch demonstrates an owned private-test seam, correct separation of
  contract/integration/characterization targets, and focused selection without
  bypassing the real production boundary.

### Stage 5: Migrate Viewport as the cross-module feature pilot

- [ ] Coordinate with the active viewport-presentation plan so target
  boundaries follow the shipped Engine/MonaCore/Mona contracts rather than the
  pre-refactor include graph.
- [ ] Establish narrow Mona viewport/display-source contract coverage and
  Engine scene-viewport contract coverage where each module owns a real public
  contract.
- [ ] Retain a cross-module `feature/viewport` target for Level Editor
  composition, main/auxiliary view behavior, camera preview, interaction, and
  same-frame resize behavior.
- [ ] Retain backend/system integration targets for Vulkan output,
  window-backed Present, offscreen sampling, and shutdown where required.
- [ ] Remove foreign LevelEditor production-private `.cpp` compilation from the
  migrated feature target by linking real feature modules or introducing
  LevelEditor-owned test seams/components with explicit ownership.
- [ ] Preserve GPU/renderer locks, heavy-runtime rationale, editor-only
  availability, preview data, and runtime-only backend dependencies.
- [ ] Validate the Mona and Engine contract targets, selected viewport feature
  set, Vulkan/integration set, editor/runtime smoke, and the full build required
  for the user-visible viewport change.
- [ ] Run the complete native aggregate only when required by the viewport plan
  gate, shared test-infrastructure changes, or concrete cross-target evidence;
  target migration alone is insufficient reason.

#### Acceptance Gate

- Viewport proves that one user-visible feature can span module contracts,
  editor composition, and backend integration while remaining selectable,
  honestly linked, and free of foreign private-source compilation.

### Stage 6: Enforce forward migration and publish the long-tail workflow

- [ ] Require structured declarations for new targets and for legacy targets
  undergoing the defined substantive changes.
- [ ] Add a concise migration report that lists remaining legacy targets and
  foreign-private-source exceptions from configured metadata without making a
  manually maintained checklist authoritative.
- [ ] Document how to select routine, risk-expanded, backend, process, and
  aggregate validation sets and how to reproduce them with named targets.
- [ ] Define scheduled/nightly and release aggregate responsibilities in the
  repository-owned CI guidance when that owner exists; keep local handoff rules
  focused and risk-based.
- [ ] Remove the broad `durin_add_engine_functional_test` helper only if no
  remaining legacy target uses it; otherwise leave it explicitly legacy and
  unable to declare new structured targets.
- [ ] Update `Current Status` with representative migration evidence and defer
  the remaining target-by-target cleanup to touch-driven maintenance.
- [ ] Run documentation validation, CMake policy probes, DurinDevTool tests,
  representative selected sets, and the complete native aggregate because the
  final enforcement changes shared test policy.

#### Acceptance Gate

- New test work cannot deepen the legacy structure, representative contract,
  feature, integration, and characterization migrations are proven, focused
  set selection is documented and reliable, and remaining legacy migration is
  visible but does not block ordinary development.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Legacy configure | Existing untouched helpers and targets retain names, availability, discovery, locks, and focused execution | CMake policy probes and representative targets |
| Structured declaration | Valid kind/domain metadata becomes target properties, reserved labels, and one deterministic registry record | CMake unit/policy probes |
| Invalid metadata | Missing kind/domain, invalid values, reserved-label injection, and characterization misuse fail at configure time | Negative CMake probes |
| Registry identity | Registry matches the active preset/configuration and stale state is rejected | CMake generation and DurinDevTool tests |
| Single target | Existing `test --target <name>` behavior remains unchanged | DurinDevTool regression tests |
| Selected set | Predicate resolves visibly, builds only selected targets, and runs their CTest direct registrations | DurinDevTool and integration tests |
| Routine UX | Batch and interactive `test <selection> [filter]` forms resolve identically and routine help contains the common workflows first | Registry/parser and shell tests |
| Diagnostic UX | Isolation requires a narrow filter and maps it to case registrations without requiring ordinary users to supply granularity plus CTest regex | DurinDevTool command tests |
| Stress/report UX | Random scheduling and JUnit reporting use named scenarios, safe defaults, and reproducible output while compatibility flags still work | DurinDevTool runtime tests |
| Error recovery | Invalid combinations fail before build and include one copyable corrected command | Parser/core tests |
| Empty/unavailable set | Command fails explicitly and never falls back to aggregate execution | DurinDevTool tests |
| Locks and timeout | GPU, legacy renderer, target serialization, and timeout policies survive selected execution | CTest policy probes and locked selection smoke |
| Characterization | Ordinary selectors exclude characterization; explicit mode retains its custom lifecycle | CMake and DurinDevTool tests |
| Runtime closure | Selected targets deploy the same derived DLL/file closure as focused execution | Runtime-closure probes |
| World pilot | Feature selection works with a low-dependency runtime lifecycle target | World focused and selected-set tests |
| Launch pilot | Contract, process integration, and characterization are separated without compiling foreign private sources | Launch focused/process tests |
| Viewport pilot | Module contracts, cross-module feature behavior, and Vulkan/system integration remain distinct and selectable | Viewport contract/feature/integration tests |
| Aggregate | Shared infrastructure changes retain default target-granularity aggregate behavior and result reporting | `test --target all` plan gates |
| Long-tail migration | Remaining legacy targets are generated from configured state and new targets cannot enter legacy helpers | Repository policy and migration report |

## Definition of Done

- CMake owns a validated structured native-test taxonomy and generates a
  deterministic configured-target registry without a handwritten catalog.
- DurinDevTool can list, explain, build, and run bounded selected target sets
  while preserving CTest lifecycle, locks, timeout, shuffle, diagnostics, and
  JUnit behavior.
- Routine batch and interactive test commands share a short positional syntax;
  users do not need to understand target/case/hybrid granularity or choose
  between GoogleTest and CTest filtering for ordinary work.
- Isolation, randomized stress, and report/CI behavior remain available through
  explicit scenarios, and obsolete hybrid/include-direct inputs are absent from
  routine help with a documented compatibility path.
- Existing focused and aggregate commands remain compatible.
- New targets and substantively changed legacy targets use the structured path;
  untouched legacy targets may migrate gradually.
- World, Launch, and Viewport establish working patterns for a low-dependency
  feature, an owned private-test seam, and a cross-module/backend feature.
- Migrated cross-module targets do not compile foreign production-private
  sources directly.
- Native-test documentation defines contract/feature/integration ownership,
  selection metadata, compatibility migration, and risk-based validation.
- Full builds and full native-test aggregates have distinct documented roles;
  routine production work uses the smallest reliable target/set closure.
- Shared infrastructure stages pass their required full aggregate gates, while
  ordinary representative migrations demonstrate focused validation.
- Remaining legacy targets and exceptions are visible through generated state,
  and their gradual migration is not required for this plan to complete.

## Deferred Follow-ups

- Automatic source-diff-to-test impact analysis after structured metadata and
  selected-set usage provide enough evidence to design it reliably.
- Historical duration-aware scheduling or test sharding across CI workers.
- Physical relocation into `Modules/`, `Features/`, and `Integration/`
  directories where it improves local ownership after target metadata is
  established.
- Migration of every remaining legacy target and removal of all legacy helpers.
- General production-private seam cleanup outside representative Launch and
  Viewport slices.
- Per-target flaky-test quarantine, retry, or statistical reliability policy.
- Hosted-CI provider configuration beyond repository-owned selection and
  aggregate contracts.

## Related Documentation

- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Viewport Presentation Decoupling](ViewportPresentationDecoupling.md)

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `CMake/Project/ProjectOutputs.cmake`
- `CMake/Tests/NativeTestDiscoveryPolicyTests.cmake`
- `CMake/Tests/NativeTestPolicyFailureProbe.cmake`
- `CMake/Tests/NativeTestRuntimeClosureTests.cmake`
- `Engine/Tests/Native/CMakeLists.txt`
- `Engine/Tests/NativeTestSupport/CMakeLists.txt`
- `Engine/Tests/Native/EngineTests/CMakeLists.txt`
- `Engine/Tests/Native/CoreTests/CMakeLists.txt`
- `Engine/Tests/Native/AssetCoreTests/CMakeLists.txt`
- `Engine/Tests/Native/RenderCoreTests/CMakeLists.txt`
- `Engine/Tests/Native/VulkanRHITests/CMakeLists.txt`
- `Tools/DurinDevTool/durin_dev_tool/build/config.py`
- `Tools/DurinDevTool/durin_dev_tool/build/core.py`
- `Tools/DurinDevTool/durin_dev_tool/build/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/build/runtime.py`
- `Tools/DurinDevTool/tests/`
