# DFunction Declaration and Defaults Plan

Summary: Accept native default arguments and ordinary C++ overloads beside one reflected function, then add explicit reflected parameter defaults without widening pointer or reflected-name contracts.

Last reviewed: 2026-10-17

Status: Archived
Completed: 2026-10-17

## Current Status

All three stages are implemented. Stage 1 was committed as 02850a22c and
Stage 2 as 12f808664; Stage 3 was tested on 12f808664 plus the implementation
changes recorded with this completion. Validation used windows-msvc-x64,
Win64-Debug-DurinEditor, and MSVC outside the sandbox after a confirmed sandbox
C1902 failure. Later completion edits affect documentation only.

- Stage 1: 113 DHT function tests and 54 native function tests passed.
- Stage 2: 118 DHT function tests and 55 native function tests passed.
- Stage 3: full DHT suite passed 357 tests; one unrelated directory-symlink test
  was skipped because the Windows token lacks symlink privileges. After final
  review, the expanded function selection passed 147 tests, including the changed
  literal classifier and all added declaration/default tests.
- CoreObjectTests passed all 173 tests (`Build/DFunctionTests/stage3-core-all.xml`).
  After the final unsigned-negation regression and fixture update, all 60
  DFunctionReflectionTests passed (`Build/DFunctionTests/stage3-reviewed.xml`).
  The unchanged other suites retain the earlier full-target evidence.
- EditorPropertyTests selection `FEditorFunctionTests.*` passed 14 tests
  (`Build/DFunctionTests/stage3-editor.xml`). An earlier incorrect filter selected
  zero tests and is not counted as evidence.
- Sandbox and RoadWeaverEditor builds passed, including RoadWeaver runtime;
  editor-test dependencies built CoreDObject, Engine and DurinEd. The affected
  selector expanded to all because DHT is outside registered runtime modules.
  Bounded coverage was chosen because the changed runtime behavior is owned by
  CoreDObject, all handwritten function records are in the tested targets, and
  PointLightComponent is the only production annotated consumer. It has no
  parameters/defaults, so the final literal-classifier correction does not alter
  these tested production generated outputs or consumer binaries.
- Changed-document validation and all-plan validation passed. Runtime and tooling
  contracts now document the implemented behavior. Raw reports/logs are retained
  under ignored `Build/DFunctionTests/` and `Build/.agent-state/logs/`.

Final review retained target-dependent unsigned negation of large nondecimal
integer literals as native-only: normalizing `-0xffffffff` to signed arithmetic
would change its native value on common targets. Explicit safe LL literals remain
supported. Exact method selection also preserves top-level const return types.
There are no remaining implementation or acceptance gates. No all build,
additional preset, GPU run, or application-hosted test was required.

## Goal

Make reflected declarations less intrusive to ordinary C++ APIs, then provide
typed, inspectable defaults for future reflection callers. Preserve a single
reflected function per name, exact native signatures, inherited function slot
identity, synchronous invocation, and existing parameter lifetime/GC rules.

Authoritative implemented contracts remain in
[Native Function Reflection](../../../Runtime/ObjectSystem/FunctionReflection.md) and
[Reflection Generation](../../../Development/Tooling/ReflectionGeneration.md).
This plan selects future behavior; update those documents as stages land.

## Selected Contract

### Declaration compatibility

- Accept default arguments on otherwise supported annotated method signatures.
  Ordinary C++ callers use normal C++ default-argument rules. The native compiler
  remains responsible for expression validity, trailing-default rules, and
  ambiguous ordinary calls.
- Extract parameter types and names independently of their initializer tokens.
  Default expressions must not expand DHT's hermetic semantic inputs or require
  parsing function bodies. Expressions whose meaning comes from ordinary
  includes must not prevent extraction of an independently supported signature.
- Keep C++ default arguments out of signature compatibility and override checks.
  Their presence must also stop invalidating otherwise compatible unannotated
  virtual overrides.
- Permit ordinary unannotated overloads in the same declaring class as one
  reflected method. Reject two annotated methods with the same name, including
  overloads distinguished by constness. Reject conflicting non-method members.
- Keep current ancestor-name hiding and incompatible override diagnostics.
  Relaxing same-class overloads does not authorize new overloads against an
  inherited reflected slot or change effective function lookup.
- Retain non-object raw-pointer, static/free function, reference-return,
  rvalue-reference, variadic, template, and DSTRUCT-method restrictions. This work
  does not add alternate reflected names or signature-based lookup.

### Reflected defaults

- Distinguish three states per parameter: no declared default; native-only
  default; reflected default available. An unsupported reflection default must
  not reject a declaration accepted by the declaration-compatibility stage.
- Initially reflect bool, fixed-width integer, float/double, string literals for
  supported string parameters, reflected enum constants, and null raw object
  pointer defaults. Support these for value and const-reference input modes.
  Decode and normalize literals with checked type/range rules; do not evaluate
  arbitrary C++ expressions in DHT. Named constants, calls, macros, struct/math
  constructors, containers, object wrappers, and non-null object expressions
  remain native-only in this increment. Any unsupported spelling is explicitly
  unavailable rather than guessed or silently converted to a type default.
- Restrict reflection application to input parameters; mutable-reference
  parameters and return slots never receive authored defaults. A legal C++
  default outside that subset remains native-only.
- Publish presence, availability, and typed value inspection on the function's
  parameter metadata. Use one authoritative default record per parameter rather
  than a second signature or an editor-only string metadata convention.
- Apply defaults explicitly to a caller-selected set of omitted input indexes
  in an owned, initialized FFunctionParameterBuffer. Preflight the entire request
  before writes: reject invalid/duplicate indexes, non-input slots, missing
  defaults, and native-only defaults with a parameter-specific result. Use
  existing typed assignment and lifetime rules; define exception behavior
  consistently with existing native value operations, without promising rollback
  for arbitrary native exceptions.
- Buffer construction retains its current native value initialization.
  ProcessEvent continues requiring a complete correctly laid-out buffer and
  never interprets zero/null as omission or applies defaults implicitly. Explicit
  supplied values, including zero/null, are untouched by defaults for other slots.
- Defaults belong to the selected effective descriptor. An annotated override
  publishes its own declaration defaults without merging ancestor defaults;
  an unannotated virtual override retains the inherited descriptor and defaults.
  Native direct calls retain C++ static-declaration default selection.
- Runtime defaults must remain available in non-editor builds. Descriptor and
  default value ownership follow module lifetime; inspection cannot create a
  persistent binding. Defaults do not participate in override type compatibility.
- Preserve void/no-argument CallInEditor admission. No parameter forms, scripting,
  RPC, delegates, static libraries, or persistent function identities are added.

## Implementation Stages

Stage 3 API selection (before shared-record edits): FFunctionParams gains a
trailing sparse default-record pointer/count. DFunction copies these records and
owns immutable typed values through shared native storage. GetParameterDefault
returns the selected parameter's record (null means no declaration), and
GetParameterDefaultValue<T> performs checked typed inspection. Owned buffers expose
ApplyDefaults for an explicit index span with preflight errors. Generated typed
factories validate exact native property identity, including enum identity and
null-only raw object values. No second parameter property graph is introduced.
Consumer search covered Engine, Sandbox, and RoadWeaver source/test roots:
handwritten records occur in CoreObjectTests and EditorFunctionActionTests;
project modules consume generated registration through the production writer.
Trailing defaults preserve omitted-default aggregate construction for those
handwritten callers. Export summaries add optional expression lists and advance
their schema; normal tool fingerprints invalidate generated-output caches.

### Stage 1: Accept default argument declarations

Dependency: selected declaration contract above.

- [x] Separate top-level parameter initializer extraction from type/name parsing
  in DHT, handling nested parentheses, braces, template arguments, and strings.
  Preserve source locations and ordinary signature diagnostics.
- [x] Inspect the Clang parse/rewrite boundary and remove default-expression
  semantic dependency where necessary without broadening include visibility or
  losing source-token information needed by Stage 3.
- [x] Apply the same signature extraction to annotated methods and unannotated
  inherited overrides. Keep exports and signature equality independent of defaults.
- [x] Add parser regression cases for named parameters with literal and complex
  defaults, include-dependent expressions, const-reference defaults, virtual
  overrides, and existing unsupported parameter types.
- [x] Compile and invoke a generated fixture whose declared default is nonzero:
  direct C++ omission uses that default, while explicit reflection arguments and
  ordinary buffer value initialization preserve existing behavior.
- [x] Update the owning contracts to describe declaration support separately
  from reflected default availability, which remains absent at this stage.

Completion: default declarations no longer block supported signature extraction;
focused DHT coverage and generated native invocation pass. Reflection dispatch
still has no implicit or explicit authored-default operation.

### Stage 2: Allow ordinary C++ overloads beside one reflected entry

Dependency: Stage 1, including tests combining overloads and defaults.

- [x] Replace blanket same-name rejection with a single-reflected-entry rule.
  Preserve duplicate-annotation, ancestor hiding, and final-override diagnostics.
- [x] Generate an exact typed method selection and invoke that selected member
  pointer. Preserve constness, reference modes, noexcept compatibility, private
  access through the existing friend, and ordinary virtual dispatch. Ensure
  signature checks still reject extraction mismatches rather than permitting
  implicit argument or return conversions.
- [x] Cover ordinary overloads with different arity/types, const/non-const pairs,
  defaults, unsupported reflection-only parameter shapes on unannotated methods,
  and private/virtual/noexcept annotated methods. Include a native case where
  name-based calling from mutable buffer lvalues would select the wrong overload.
- [x] Verify duplicate reflected names and inherited hiding still fail with
  useful source locations; effective enumeration and CallInEditor identities
  remain one slot per reflected name.
- [x] Update authoring and generation documentation for the supported coexistence.

Completion: generated native calls reach exactly the annotated overload, including
virtual overrides, and all retained rejection/lookup regressions pass.

### Stage 3: Publish and apply reflected parameter defaults

Dependency: Stages 1 and 2. Before editing shared records, map consumers across
every project in Durin.dworkspace and record the chosen API/record shape here.

- [x] Add default presence/availability and normalized typed literal records to
  the DHT model. Preserve native-only expression provenance for diagnostics
  without turning expression text into executable runtime input.
- [x] Implement the selected literal subset with range, escaping, enum identity,
  and null-object checks. Add deterministic native-only diagnostics for other
  forms, including nested/complex expressions accepted by Stage 1.
- [x] Propagate default records through symbol exports, dependency fingerprints,
  generation caches, and generated C++ metadata. Version persisted formats where
  required; verify a default-only edit invalidates relevant generated consumers
  without becoming an override signature mismatch.
- [x] Extend generated and handwritten function records and function Link
  validation. Own typed default data under the descriptor, seal it with linked
  metadata, and reject invalid kinds, roles, enum identities, and record indexes
  before publication. Migrate all shared-record consumers and fixtures.
- [x] Add metadata inspection and explicit owned-buffer default application with
  request preflight and parameter-specific failure results. Keep dispatch and
  buffer-construction semantics unchanged.
- [x] Test every supported kind, zero versus absent default, string lifetime and
  escaping, integer bounds, enum identity, null objects, native-only availability,
  mutable references, return exclusion, and rejection without partial writes
  for invalid requests. Verify explicitly supplied zero/null values survive.
- [x] Test annotated/unannotated virtual override default selection, descriptor
  lifetime, default-only regeneration, and generated invocation using defaults
  followed by caller overrides. Preserve parameter rooting and existing GC tests.
- [x] Document final runtime/default ownership, inspection/application, accepted
  literal subset, native-only states, override rules, and unchanged editor admission.

Completion: callers can inspect available defaults, explicitly fill omitted
supported inputs, and invoke through ProcessEvent; unsupported/missing defaults
are observable failures and never implicit type initialization.

## Validation and Handoff

Follow [Build And Run](../../../Agents/BuildAndRun.md) before any native build and
[Testing](../../../Agents/Testing.md) for target discovery, execution, and result reuse.
Use one source/build writer; do not overlap build process trees.

- [x] Run focused DHT function parsing/generation tests for each affected stage,
  extending export/cache coverage where default records change those paths.
- [x] Discover and run the relevant CoreDObject native function tests and generated
  fixture coverage. Check editor function-action regressions for preserved slot
  lookup and void/no-argument admission when those paths are affected.
- [x] For shared generated/runtime API changes, search and migrate all consumers
  in Engine, Sandbox, and RoadWeaver, then build the owning module and affected
  consumer targets in each project. Use an all build only if ABI/header/build-graph
  impact cannot be bounded; explain the uncovered risk before expanding scope.
- [x] Validate changed documents and all-plan lifecycle links using the repository
  documentation workflow. No native build is required for this initial plan edit.
- [x] Record tested revision, selections, configuration, results, and remaining
  gates in Current Status. Update checklists with evidence in each implementation
  commit and use the exact Plan/Stage commit trailers required by repository rules.
- [x] After all gates pass, publish lasting contracts in their owning documents
  and complete this plan using the plan lifecycle workflow.
