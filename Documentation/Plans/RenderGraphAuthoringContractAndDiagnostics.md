# Render Graph Authoring Contract and Diagnostics Plan

Summary: Complete parameter-driven authoring enforcement, inspection, diagnostics, compatibility disposition, and lasting contracts.

Last reviewed: 2026-08-29

Status: Completed
Completed: 2026-08-29

## Current Status

Milestones 1--4 are complete. Production scene authoring consists of thirteen
parameterized passes owned by twelve narrow contributors, and no production
Renderer caller supplements those passes with `FRenderGraphBuilder::Use*`.
The remaining manual builder surface is exercised only by RenderCore compiler
oracle tests and Vulkan transition tests.

Stages 0 and 1 are complete. `FRenderGraphCapture` now owns stable submitted
parameter-structure and leaf-field capabilities for retained and culled passes,
including absent optional fields, member kind, canonical resource identity,
exact range/access intent, managed transition result, and composed shader
binding. Dumps expose the same pointer-free evidence, while normalized uses and
dependencies remain canonical compiler output. `RenderContractTests` passes
all 94 cases after adding array, nested, optional, attachment, managed resource,
typed-value, fallback, shader-composition, lifetime, deterministic, and
field/resource/use correlation assertions. Stage 2 is also complete. The
Renderer contract target now scans every production Renderer C++ source file,
rejects manual `Use*` calls and `AddPass` without a moved parameter object, and
proves both seeded rejection and the current contributor chain. Its capture
observer fixture now uses a parameterized output pass and asserts the owning
field inspection record. The lasting Render Graph contract bounds the manual
surface to RenderCore compiler and Vulkan transition oracles and records the
removal gate. `RendererSceneContractTests` passes all 40 cases. Stage 3 is
complete after auditing malformed metadata, foreign/range/domain
uses, overlapping fields, external import conflicts, composed shader binding,
and pass-scoped resolution. Existing diagnostics already carried the applicable
struct/member, pass/field/resource, canonical/conflicting import, and
pass/field/binding context. The two proven gaps are repaired: overlap failures
now name both conflicting parameter fields, and resolver failures now name the
executing pass plus requested capability and optionality. Repeated overlap
declarations produce identical text. `RenderContractTests` passes 94 cases and
`RenderShaderContractTests` passes 41 cases, including their existing atomic
pre-recording rejection coverage. Stage 4 is next.

Stage 4 and the plan are complete. The lasting Render Graph and Shader
Parameters contracts now define parameter-field inspection, field/use/
dependency correlation, optional absence, the production source enforcement
gate, bounded compatibility ownership, and the new-shader capture checklist.
Final validation passes `RenderContractTests` (94),
`RenderShaderContractTests` (41), `RendererSceneContractTests` (40),
`RendererResourceReloadVulkanTests` (1), `EditorGridVulkanTests` (7), the exact
`DirectionalShadowBaselineVulkanTests` qualification selection, and the full
Win64 Debug DurinEditor `all` build. Documentation lifecycle validation passes.
No entry evidence demonstrates material transient-memory pressure, independent
queue overlap, or a dominant CPU/render-pass bottleneck, so roadmap Milestones
6--8 are explicitly deferred rather than activated.

## Goal

Make parameter-driven Render Graph authoring the enforceable production
contract and expose enough pointer-free inspection evidence to explain every
pass capability, selected or absent resource, normalized use, shader binding,
and resulting dependency without consulting callback code.

## Scope

- RenderCore capture records and deterministic dumps for submitted parameter
  structures, fields, optional engagement, member kind, resource identity,
  normalized capability, and composed shader binding.
- Cross-pass inspection fixtures covering a representative production-style
  producer/consumer chain and selected fallback visibility.
- Repository structural enforcement that production Renderer passes remain
  parameterized and do not call the low-level manual declaration surface.
- Deterministic validation and diagnostics for pass, field, resource,
  capability, and conflicting declarations where current coverage is absent.
- Explicit compatibility disposition for manual `AddPass` and `Use*` APIs.
- Lasting Render Graph and shader-parameter guidance, examples, and validation
  ownership.

## Non-Goals

- Transient physical aliasing, queue-aware scheduling, split barriers,
  parallel recording, pass merging, compiler reordering, or persistent graphs.
- A generic editor debugger, persisted capture file format, remote telemetry
  protocol, or public plugin pass registry.
- Rewriting compiler-oracle and Vulkan transition tests solely to remove the
  low-level manual API they intentionally validate.
- Changing scene topology, shader algorithms, draw/dispatch identity,
  fallback policy, RHI state authority, or rendered output.

## Design Decisions and Invariants

- `FRenderGraphCapture` gains owning parameter-field records rather than
  reconstructing schemas from normalized uses. This preserves absent optional
  capabilities and avoids exposing metadata or parameter-storage pointers.
- Each field record uses the pass declaration index and stable field path as
  its identity. Engaged resource fields also carry the canonical resource ID;
  absent optionals carry no synthetic resource.
- Field records describe declared authoring capability. Existing use records
  remain the compiler-normalized execution evidence, and dependencies remain
  canonical compiler output. Inspection connects these records without
  introducing a second lowering or dependency model.
- Captures and dumps remain deterministic and pointer-free. No allocation ID,
  address, timestamp, callback state, or measured duration becomes identity.
- The manual builder surface remains public low-level compatibility for
  compiler semantics, backend transition qualification, and bounded migration
  fixtures. Runtime Renderer production source may not call it; a structural
  contract test makes that boundary executable.
- Parameterized production passes remain the only supported path for new
  inter-pass work. Tests may use the low-level oracle only when their purpose
  is to validate canonical compiler or RHI transition behavior.
- Diagnostics are repaired only where new focused tests prove that pass,
  parameter field, stable resource, declared capability, or conflicting
  declaration context is missing.
- Existing compile-before-recording failure, immutable graph storage,
  deterministic schedule, culling, backing publication, RHI authority, and
  frame commit/abort contracts do not change.

## Current Foundations and Gaps

| Area | Foundation | Remaining gap |
| --- | --- | --- |
| Production authoring | All scene passes are parameterized and contributors are narrow | No executable source contract prevents a future manual declaration from returning |
| Use inspection | Captures own normalized uses, field paths, shader bindings, dependencies, transitions, and resources | Absent optionals and the complete submitted field capability set disappear during lowering |
| Pass inspection | Captures own pass name, type, declaration index, and transition counts | The submitted parameter structure is not named on the pass record |
| Diagnostics | Parameter lowering prefixes validation with pass and field paths | Coverage does not systematically assert pass/field/resource/capability context across malformed and conflicting cases |
| Compatibility | Runtime guidance calls manual APIs compatibility-only | The allowed low-level consumers and production prohibition are not enforced |
| Lasting guidance | Render Graph and Shader Parameters describe the implemented foundation and scene migration | Inspection workflow, compatibility boundary, and new-feature checklist are incomplete |

## Implementation Stages

### Stage 0: Freeze inspection and compatibility boundaries

- [x] Inventory production and test callers of manual `AddPass` and `Use*`.
- [x] Confirm that production scene passes are parameterized and that manual
  declarations remain useful only as low-level compiler/backend test oracles.
- [x] Select owning pointer-free field captures rather than exposing live
  metadata, parameter storage, or callback objects.
- [x] Freeze the enforcement, documentation, focused test, Vulkan qualification,
  and full-build validation lanes.

#### Acceptance Gate

- Scope, decisions, compatibility ownership, and validation requirements are
  explicit, and the roadmap links this active bounded plan.

### Stage 1: Add parameter-capability inspection

- [x] Add stable parameter-structure identity to pass captures.
- [x] Add owning field capture records for nested members, fixed arrays,
  engaged and absent optionals, typed values, attachments, managed resources,
  and composed shader fields.
- [x] Connect engaged fields to canonical resource IDs and normalized uses
  without duplicating compiler decisions.
- [x] Extend deterministic dump output with a bounded parameter-capability
  section.
- [x] Add focused RenderCore fixtures proving ownership after graph destruction,
  deterministic equality, absence semantics, field/resource/use correlation,
  and production-style producer/consumer inspection.

#### Acceptance Gate

- A capture explains the complete immutable capability submitted to every
  parameterized pass, including capabilities intentionally omitted by route or
  fallback selection, and focused RenderCore tests pass.

### Stage 2: Enforce the production authoring boundary

- [x] Add a Renderer structural contract that rejects non-parameterized
  production pass declaration and manual `Graph.Use*` calls while excluding
  intentional test oracles and RenderCore implementation.
- [x] Prove the contract against all scene contributors and the stable
  directional-shadow-through-output chain.
- [x] Document the exact low-level compatibility consumers and the criteria
  for eventually removing the manual surface.
- [x] Verify scene capture and renderer contract coverage still observes the
  parameterized production route.

#### Acceptance Gate

- A repository test fails if manual Render Graph authoring re-enters production
  Renderer source, while intentional compiler and Vulkan oracle tests remain
  supported.

### Stage 3: Complete actionable diagnostics

- [x] Audit malformed metadata, wrong domain/access, foreign handle,
  overlapping field, conflicting external registration, shader composition,
  and pass-scoped resolution failures against the required context.
- [x] Add focused negative fixtures for every context gap before changing its
  implementation.
- [x] Make repaired errors name the stable pass, field, resource, capability,
  and conflicting declaration as applicable without pointer identity.
- [x] Confirm equal invalid declarations produce equal messages.

#### Acceptance Gate

- Focused RenderCore and shader contract fixtures prove actionable,
  deterministic failures for the supported authoring mistakes, with no RHI
  command recorded after pre-recording rejection.

### Stage 4: Publish contracts and qualify the production route

- [x] Update Render Graph and Shader Parameters with inspection usage,
  compatibility ownership, and the checklist for new inter-pass work.
- [x] Update the roadmap and plan with bounded structural, capture, rendered,
  recovery, and performance evidence.
- [x] Run focused and aggregate RenderCore/Renderer contract tests, the required
  Vulkan directional-shadow route qualification, documentation validation, and
  a full `all` build under the repository workflows.
- [x] Complete and archive-ready this plan, then disposition Milestones 6--8
  strictly from their independent measured entry gates and complete the
  roadmap if none is justified.

#### Acceptance Gate

- Lasting contracts are authoritative; parameter inspection and enforcement
  pass focused and aggregate validation; production rendered behavior,
  recovery, budgets, and the full build pass; conditional optimization work is
  explicitly selected or deferred.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Capture ownership | Focused RenderCore test reads parameter records after compiled graph destruction |
| Field completeness | Nested, array, optional, value, attachment, managed texture, texture, buffer, and token fixtures |
| Correlation | Stable pass declaration index, field path, resource ID, normalized use, and shader binding assertions |
| Determinism | Repeated equal valid and invalid declarations produce equal captures, dumps, and messages |
| Enforcement | Renderer source contract rejects a seeded manual-authoring violation and accepts current contributors |
| Compiler/RHI preservation | Focused RenderCore aggregate and Vulkan resource-transition tests |
| Production wiring | Renderer scene contracts and directional-shadow compute/fragment capture qualification |
| Runtime parity | Required offscreen/window output, recovery, and representative rendered fixtures selected by Stage 4 |
| Performance | Existing scene structural and CPU budgets plus parameter capture construction cost remain within frozen gates |
| Documentation/build | Changed/all documentation validators and a successful final `all` build |

## Definition of Done

- Every parameterized pass exposes an owning pointer-free record of its
  submitted structure and complete field capability set.
- Captures correlate fields with canonical resources, normalized uses, shader
  bindings, dependencies, and selected absence without reconstructing compiler
  behavior.
- Production Renderer source is structurally prevented from using manual pass
  declarations or `Use*` supplements.
- Supported authoring failures are deterministic and actionable at the
  pass/field/resource/capability boundary.
- Manual APIs have one authoritative, explicitly bounded compatibility purpose.
- Lasting Render Graph and Shader Parameters contracts, focused and aggregate
  tests, Vulkan/rendered/recovery evidence, performance budgets, documentation
  validation, and the full build pass.
- The roadmap is completed or links only separately justified conditional
  optimization plans.

## Deferred Follow-ups

- Physical transient aliasing remains Milestone 6 and requires measured peak
  memory pressure plus Vulkan placement/completion prerequisites.
- Queue-aware scheduling and split barriers remain Milestone 7 and require
  independent queue/timeline contracts plus measured overlap.
- Parallel recording, pass merging, reordering, and persistent graph reuse
  remain one-technique Milestone 8 plans selected only from a measured
  bottleneck.
- A persisted or interactive capture UI is separate tooling work after an
  actual consumer and format/lifetime requirements exist.

## Related Documentation

- [Render Graph Parameter-Driven Authoring Roadmap](../Roadmaps/RenderGraphParameterDrivenAuthoring.md)
- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Build and Run](../Agents/BuildAndRun.md)
- [Testing](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphContributors.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphComposer.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
