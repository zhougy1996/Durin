# Material Input Constants Plan

Summary: Group numeric material connections and explicit constants, separate definition defaults from authored overrides, and unify input resolution and editing.

Last reviewed: 2026-09-20

Status: Archived
Completed: 2026-09-20

## Current Status

Stages 1–4 are complete. Numeric expressions, terminal Surface properties, UVs
and individual Surface overrides use reflected grouped inputs. Runtime definition
fallbacks replace catalog-initialized defaults, and editor commands preserve the
connection, flag and retained value through reset, graph edits and transactions.
The lasting contracts are documented in [expression building](../../../Runtime/Rendering/MaterialExpressionBuilding.md#numeric-input-ownership)
and [graph operations](../../../Editor/Architecture/MaterialGraphOperations.md#compact-input-and-texture-authoring).

The two materials and three standard functions were converted through exact-package
canonical resave. All five writes and verification passed; temporary old-layout
readers and deprecated fields were then removed. Final graph version is 4, output
version is 3 and clipboard schema is 9. Existing explicit defaults, including
connected retained values, became enabled constants; none were inferred from
comparison with a system default. The standard functions still match their recipes.

Validation on `Win64-Debug-DurinEditor`:

- `.\DevTool.bat build --target all` passed after the final source changes.
- `.\DevTool.bat test "@domain=material,kind=feature" --test-jobs 4 --report`
  passed all 11 targets, covering compiler, Cook, functions, packages, editing,
  persistence, preview, lifecycle, runtime, thumbnails and mesh binding.
- `.\DevTool.bat test SceneImportTests --report` passed all 10 cases.
- `.\DevTool.bat test MaterialEditorInteractionTests` passed all 66 cases after
  consolidating preview Surface initialization into the authoritative fallback.
- `.\DevTool.bat asset material-functions --project Sandbox/Sandbox.dproject`
  verified unchanged expression counts and exact standard-function recipe parity.
- `.\DevTool.bat asset check --project Sandbox/Sandbox.dproject --json` and
  `.\DevTool.bat asset check --project RoadWeaver/RoadWeaver.dproject --json`
  reported all 19 / 10 packages Compatible and Current, respectively.
- The existing package-version tests reject missing and old graph/output versions;
  round-trip and duplication tests preserve connected explicit values. New tests
  cover connection precedence with either flag, explicit zero, disabled malformed
  storage, equal-to-default overrides, reset/re-enable and Undo/Redo.
- `test affected --explain` resolved to the whole workspace because shared Engine
  headers changed. The material domain plus SceneImport was selected for semantic
  coverage; the required `all` build covered the shared API consumers.
- `.\DevTool.bat doc validate --scope changed`,
  `.\DevTool.bat doc plan validate --scope all`, and `git diff --check` passed.


## Goal

Make an authored numeric input own its connection and retained explicit constant,
while its node or material-attribute definition owns the system fallback.
Preserve material behavior, numeric widths, broadcast rules, diagnostics,
transactions, and stable graph identities during the transition.

## Selected Design and Scope

### Input representation

- Keep `FMaterialExpressionInput` as the identity-only connection representation.
- Introduce concrete reflected numeric input structures containing `Connection`,
  `UseConstant`, and `Constant`. `UseConstant` defaults to `false`.
- Use `FMaterialInput<InputType>` as a conceptual model, not a requirement to
  support arbitrary reflected C++ templates. Concrete scalar/vector/adaptive
  representation is selected in Stage 1 under existing DHT constraints.
- Preserve adaptive numeric widths and scalar broadcast. Do not encode every
  numeric input as a fixed-width float or vector solely for template convenience.
- Keep texture and aggregate Surface connections free of meaningless numeric
  constants. Function port defaults and material parameter defaults retain their
  distinct ownership; adapt affected consumers without merging these concepts.

### Resolution contract

| Condition | Result |
| --- | --- |
| A connection exists | Resolve the upstream expression |
| Disconnected and `UseConstant` is true | Use the retained `Constant` |
| Disconnected and `UseConstant` is false, with a definition fallback | Resolve the definition fallback |
| Disconnected with neither explicit constant nor definition fallback | Report a missing required input |

`UseConstant` controls disconnected fallback selection; it does not bypass an
existing connection. This is a Durin decision, not a claim of identical UE
precedence. A broken connection or incompatible upstream type must report an
error rather than silently fall back.

Connecting or disconnecting an input preserves both `UseConstant` and `Constant`.
Editing a literal enables `UseConstant`; resetting to the definition default
clears it and retains the stored constant for subsequent re-enablement. Reset
does not remove a connection. The editor must distinguish inherited defaults
from explicit constants, including an explicit constant equal to the default.

Definition fallbacks belong to the owning node/input or material attribute,
not to a duplicated initialization value on each pin. Defaults may require
context-dependent expressions, so preserve existing UV and normal semantics
rather than treating every fallback as zero-initialized numeric storage.

The existing `bUseMaterialAttributes` mode and pruning behavior remain intact:
disconnected aggregate Surface resolution uses standard Surface defaults,
independently of retained per-property overrides.

### Boundaries

This plan changes authored input storage and its consumers. It does not redesign
MIR operations, shader evaluation, material parameter overrides, texture fallback
policy, or general-purpose reflection. Resolve constants before emitting the
existing compiler representation where possible. Change derived-data identity
only where the affected format or semantics require it.

## Implementation Stages

### Qualification notes

- `asset material-functions --project Sandbox/Sandbox.dproject` passed against
  the original format: DefaultMaterial has one expression, M_MetalRust has five,
  and SampleNormal / SampleORM / UVTransform have 8 / 6 / 18 respectively.
  M_MetalRust owns four texture parameters; their stable IDs must survive.
- Existing storage has no `UseConstant` flag. Empty numeric arrays mean no
  fallback and fail when disconnected. AppendVector A/B and TextureCoordinates
  Channel initialize to scalar zero. The standard SampleNormal recipe explicitly
  authors Lerp A as float3 `(0, 0, 1)`; this must remain explicit.
- Catalog creation currently authors zero at the selected input width, except
  Multiply/Divide B, Clamp Maximum, and Normalize use one; Lerp uses A=0, B=1,
  Alpha=0.5. Move this policy to definition lookup, without treating old authored
  arrays equal to those values as inherited.
- Surface defaults are BaseColor=(0.5,0.5,0.5), Normal=(0,0,1), Metallic=0,
  Roughness=0.5, AmbientOcclusion=1, Emissive=(0,0,0), Opacity=1, OpacityMask=1.
  Texture UV falls back to mesh UV0; resource and aggregate connections remain
  identity-only. Function port and parameter defaults keep their own semantics.
- Use concrete reflected numeric storage with nested Connection, UseConstant,
  and a finite float array Constant. Preserve adaptive widths and validate retained
  values even when connected or disabled. Nested struct reflection is supported;
  input traversal must explicitly descend to Connection.
- Package graph/output custom versions and clipboard schema are the authored
  boundaries. Use explicit old-version rejection and controlled repository
  asset conversion; do not add broad historical compatibility. Existing MIR and
  shader encodings need no change when resolved values remain identical.
- The controlled conversion uses a temporary old-layout reader to copy each
  existing connection and nonempty default to the new grouped fields before
  canonical resave. Old nonempty arrays and all old concrete terminal defaults
  become enabled constants, including values behind connections. Empty arrays
  become disabled constants with canonical finite storage. The temporary reader
  is not part of the final runtime; final readers reject the old package version.
- Source/test consumers are in Engine (runtime Engine, MaterialEditor,
  AssetForgeBuiltins and native material suites). Sandbox and RoadWeaver are
  workspace consumers and participate in the required final `all` build.

### Stage 0: Define the refactor contract

Dependencies: the design discussion and current input/editor implementation.

- [x] Select grouped numeric inputs and separate definition fallbacks.
- [x] Define connection precedence and `UseConstant` / `Constant` semantics.
- [x] Bound the work and record migration and validation gates.

Completion: this plan records a reviewable design; implementation is not implied.

### Stage 1: Qualify defaults and compatibility

Dependencies: Stage 0.

- [x] Inventory numeric inputs, required inputs, special fallbacks, and default
  initialization in constructors, catalogs, recipes, importers, and fixtures.
  Record exact values/widths and the meaning of existing empty defaults.
- [x] Search changed symbols through source and test roots of all projects in
  `Durin.dworkspace`: Engine, Sandbox, and RoadWeaver.
- [x] Select concrete reflected structures and numeric constant representation;
  verify nested reflection, property editing, serialization, and traversal needs.
- [x] Specify one authoritative Surface-default definition and per-node fallback
  providers, including context-dependent defaults and required-input behavior.
- [x] Identify package, function, clipboard, and derived-data format boundaries.
  Record whether affected old formats receive an explicit reader migration or
  version rejection plus controlled regeneration. Existing output contracts
  reject older versions; do not assume automatic compatibility.
- [x] Define retained-value validation, including disabled constants and connected
  overrides. Preserve existing finite-value/width checks where applicable and
  use a valid canonical initialization for new inactive constant storage.

Completion: add the selected representation, fallback inventory, compatibility
policy, and affected targets to this plan before changing storage. No unresolved
decision may allow an existing explicit value to become an inherited default.

### Stage 2: Implement input storage and runtime resolution

Dependencies: Stage 1 decisions and consumer inventory.

- [x] Add concrete reflected inputs and authoritative fallback definitions.
- [x] Migrate numeric expression fields and individual Surface inputs; update
  emitter, builder, snapshots, catalog initialization, and affected consumers.
- [x] Replace parallel connection/default argument arrays with grouped input
  access; retain stable expression GUIDs, output selectors, and pin identities.
- [x] Resolve the selected fallback through one shared contract, preserving
  adaptive widths, broadcasts, required-input diagnostics, and Surface modes.
- [x] Implement the selected format transition atomically with new readers and
  writers. For migrated values, map existing explicit defaults to enabled
  constants, including defaults retained behind connected pins. Map empty
  defaults only according to the Stage 1 inventory; do not infer user intent by
  comparing an old value with the system default.
- [x] Reject unsupported old data explicitly; never load it as silently reset
  pins. Retire obsolete fields after all supported readers/consumers migrate.

Completion: runtime consumers and persistence agree on the new model, with
focused tests proving precedence, required-input errors, retained constants,
width validation, and the selected compatibility policy.

### Stage 3: Integrate graph editing and transactions

Dependencies: Stage 2 input and resolution APIs.

- [x] Replace suffix-based reflection lookup with explicit input access that
  traverses nested connection members and exposes constant state.
- [x] Update numeric and Surface editing, effective-default display, reset,
  enable/disable, type inference, and width changes using shared commands.
- [x] Preserve constants through connect/disconnect, source deletion,
  replacement, extraction/inlining, promotion, duplication, and clipboard edits.
- [x] Update function document consumers without changing function-port default
  semantics; qualify dynamic port identities and aggregate inputs.
- [x] Ensure undo/redo restores the connection, flag, and value together; preserve
  no-op/rejection behavior, dirty state, change observation, and compile scheduling.

Completion: root and function editors expose the same semantics, with focused
interaction and transaction tests and no remaining suffix-based pairing.

### Stage 4: Qualify and document the completed contract

Dependencies: Stages 2 and 3.

- [x] Test connected inputs with either flag state, disconnected inherited and
  explicit values, explicit zero, explicit values equal to defaults, reset and
  re-enable, invalid connections, required inputs, and scalar/vector broadcasting.
- [x] Test both Surface modes and property activation/pruning; compare migrated
  representative material results with their pre-refactor behavior.
- [x] Verify material/function save-load, retained connected values, clipboard,
  undo/redo, and migration or version rejection with representative fixtures.
- [x] Validate compilation, preview, and Cook through affected existing suites;
  check authoring fingerprints and cache invalidation at changed boundaries.
- [x] Build affected project targets and complete an `all` build for the shared
  Engine API migration, following the build/test workflows linked below.
- [x] Move implemented contracts to the owning runtime/editor documentation;
  record actual commands and outcomes here, close passed checks, and complete
  the plan lifecycle only after every gate succeeds.

Completion: all affected projects build, behavioral and persistence gates pass,
and long-lived documentation describes the implemented model.

## Effort and Risks

Expected scope is a medium cross-module refactor, not a field rename. Most work
is in consumer migration, reflection-based editing, and persistence qualification.
General template reflection and broad historical asset conversion are excluded;
either would materially expand scope. A reliable file count or time estimate
requires the Stage 1 inventory.

The main risks are loss of retained values, accidental new defaults for required
inputs, changes in adaptive widths, missed nested links during graph traversal,
and disagreement between authored Surface defaults and compiler snapshots.

## Required References

- [Material expression building](../../../Runtime/Rendering/MaterialExpressionBuilding.md)
- [Material graph operations](../../../Editor/Architecture/MaterialGraphOperations.md)
- [Reflection system](../../../Runtime/Core/ReflectionSystem.md)
- [Material system roadmap](../../../Roadmaps/MaterialSystem.md)
- [Build and run workflow](../../../Agents/BuildAndRun.md)
- [Native testing workflow](../../../Agents/Testing.md)
- [Documentation workflow](../../../Agents/Documentation.md)
