# RHI Public API Stability

Summary: Define the source and semantic stability boundary of public RHI headers without promising a C++ binary ABI or unsupported GPU features.

Modules: RHI, RenderCore, ShaderFormat, VulkanRHI, MetalRHI

Last reviewed: 2026-10-17

## Directory Contract

Header placement communicates the audience and compatibility expectation:

| Directory | Status | Contract |
| --- | --- | --- |
| `RHI/Public/` | Stable | Runtime and renderer consumers may depend on the documented source and semantic contract. Breaking changes require an explicit migration of every workspace consumer, owning contract updates, and affected validation in the same change. |
| `RHI/Public/Experimental/` | Experimental | Selected production or diagnostic code may exercise the API, but signatures and semantics may change while the owning active plan remains open. Callers include the header through its `Experimental/` path so that the dependency is visible. |
| `RHI/Public/Backend/` | Backend-only | RHI implementations and focused conformance fixtures may use the API. Renderer and feature code must not depend on it. It may change together with every backend consumer and is not a renderer-facing compatibility contract. |
| `RHI/Private/` | Private | Only the RHI implementation may depend on it. No cross-module source compatibility is promised. |

The root `Public` directory is the stable default. A declaration does not need a
stability macro or repeated status comment when its directory already expresses
that status. A mixed stable header may refer to an experimental type only when
the affected operation is identified below and callers must include the
experimental header transitively or directly. New experimental families should
prefer a complete header boundary rather than adding unrelated unstable members
to a stable type.

Detailed parameter-validation errors and validators live in
`Backend/RHIValidation.h`. Internal cache-capacity exceptions live in
`Backend/RHICreation.h`. Recording and execution report validation diagnostics inside
RHI. Ordinary consumers use resource descriptions and commands, or the boolean
`IsTextureViewDescValid` query when selecting attachment views. Conformance
fixtures may inspect the backend-only error codes and context.

## Frozen Meaning

Stable freezes caller-visible behavior, not implementation layout. The stable
contract covers:

- ownership, reference retention, and destruction prerequisites;
- command recording, admission, ordering, replay, and CPU/GPU completion meaning;
- resource-description, view-range, transition, copy, and binding validation;
- pipeline identity and complete-or-failure publication;
- capability-field meaning and documented fallback behavior;
- error categories, observable failure state, and thread requirements; and
- backend-neutral values consumed by Renderer and RenderCore.

Stable does not freeze a C++ binary ABI. Private data layout, vtable layout,
inline implementation, cache organization, capacity constants, Vulkan native
types, and diagnostic formatting may change without compatibility adapters.
Durin projects build from one workspace revision and do not load independently
versioned RHI client binaries.

`FShaderTargetIdentity` carries backend/code-format discriminants and an opaque
settings-profile digest. ShaderFormat owns language/remap options and their
supported immutable profiles; common RHI does not interpret those settings.
`FShaderCode` is the common compiled-stage representation: immutable shared code
bytes, entry points, target metadata, and canonical reflection declarations.
There is no separate artifact or duplicated per-stage pipeline layout.
RenderCore publishes a shared immutable code owner in ShaderMap;
`MakeShaderCreateDesc` reuses it through `FRHIShaderCreateDesc::CodeOwner`.
Code, reflection and metadata survive the source compiler output's lifetime.
Native admission validates the owner against all generic creation arguments
before publication. Common shader descriptions and pipeline keys carry the
complete `ShaderIdentity`; they contain no Metal mapping fields or accessors.
Metal requires a code owner and stores its
decoded map only in `FMetalShader`. Vulkan also supports raw SPIR-V creation
descriptors through its existing backend admission path. Backend, platform, and
code-format discriminants remain public for target selection and compatibility;
they do not expose target metadata schemas or native slot policy.

Raw creation validates the target/code format, bounded code shape, code hash,
stage, entry point, and execution metadata. A nonzero shader identity requires
an attached code owner. Owned code additionally validates resource types,
per-stage visibility, unique logical addresses, array bounds, and aligned,
bounded, non-overlapping push ranges, even if the caller recomputes the hash.
An explicitly supplied descriptor identity must match the code owner.
Owned creation compares the descriptor's fields, code hash and byte view with
the owner before validating that code once. Raw creation still validates its
inputs independently. Format, digest and stage checks share ShaderFormat helpers;
archive readers retain bounded parsing and their own precise error classification.

Pipeline-key description fields retain shared immutable RHI-owned CPU data.
The validating key builders canonicalize and intern layouts, vertex input, and
blend descriptions; consumers retain the shared references instead of copying
or mutating description arrays. Content equality and hashing remain the owning
[graphics contract](GraphicsStateAndBindings.md#graphics-pipeline-contract).

Pipeline creation requests expose `GetTaskHandle()` for CPU completion
dependencies. This erased Core handle contains no native result or cancellation
authority over shared compilation; request cancellation remains an explicit RHI
operation. The declaration depends on `Threading/Task.h`, while task/event
composition belongs to the [Core task API](../Core/TaskSystem.md#composition).

An enum value, resource flag, shader frequency, or feature level is vocabulary,
not proof that a backend implements the feature. Runtime support requires a
published capability or exact support query and a complete public creation and
execution path. Reserved ray-tracing, multi-GPU, sparse-resource, and later
feature vocabulary therefore makes no support promise.

## Current Stable Families

The stable surface comprises the root public headers and their documented
contracts, including:

- resources, descriptions, counted views, pixel formats, and resource references;
- graphics and synchronous-compute pipeline descriptions and creation results;
- command-list recording, executor admission, RHI-thread dispatch, and fences;
- exact buffer and texture transitions, copies, uploads, and readbacks;
- single-command indirect draw, indexed draw, and dispatch argument records,
  validation, recording, replay, and conservative capability fields;
- reflected shader parameters, descriptor arrays, and push constants;
- viewport presentation, initialization, capabilities, diagnostics, and
  typed pipeline-cache identities, descriptor-based binding helpers, and explicit
  pipeline-creation requests for preparation and backend qualification; and
- queue-qualified GPU completion, explicit queue ownership transfer, and the
  related `FDynamicRHI`, command-context, RDG allocation, and submission
  recording operations.

The owning semantic documents remain authoritative. This document classifies
their API surface and does not duplicate their detailed rules.

Shader creation carries the backend target, code format, binary entry point,
code hash, and an optional immutable code owner with opaque platform metadata.
Metal accepts canonical MSL slot assignments and matching native function types;
Vulkan rejects Metal binding metadata. MSL source syntax is validated by the native
compiler; include spelling, directive position and a minimum source-text length
are not admission requirements. Empty code, hash/target/format mismatches, invalid
binding metadata and incompatible native function stages remain rejected.
Pipeline creation retains complete-or-
failure publication when a required shader or binding contract is incompatible.

Presentation initialization and viewport creation share `FRHIPresentationTarget`,
which retains an immutable platform-neutral target. Backend-specific native
extensions live in platform headers and are consumed only by the matching
backend and native fixtures. Common RHI consumers forward the target without
interpreting its platform payload. Retaining target metadata does not extend the
native window's lifetime.

The unified buffer migration removes the former logical Storage creation and
update APIs. Structured and byte-address data now use native buffer factories,
views, and ordered uploads. Existing bindings do not preserve old ordinary
buffer contents; callers needing immutable alternatives retain separate buffers.
The [resource-view contract](RHIResourceViewsAndTransfers.md#uniform-and-storage-buffers)
owns this behavior; [the selected migration plan](../../Plans/Archive/2026-10/UnifiedRhiBuffers.md)
records implementation and acceptance evidence.

## Current Experimental Families

`Experimental/RHITransition.h` contains the Stage 4 split-barrier transition
object, descriptor, creation boundary, and command-list begin/end operations.
It remains experimental while native split lowering and its performance gate
are open; unsupported backends must preserve the documented full-barrier
fallback.

The queue-qualified completion and queue-ownership-transfer family was promoted
to `RHICompletion.h` and `RHIQueueTransfer.h` when Stage 3 of the multi-queue
plan was accepted for the available macOS scope. Its promotion required:

1. accepted single-queue fallback and independent-queue behavior;
2. complete resource-retirement, failure, cancellation, and shutdown coverage;
3. graph-boundary and extraction-readiness semantics;
4. one qualified production consumer or an explicit decision to freeze only
   the diagnostic/manual scheduling contract; and
5. affected native tests and the shared Engine API `all` build on the promotion
   revision.

The available-host qualification includes the Apple M4 graphics-queue fallback,
the production contact-shadow opt-in path, Vulkan validation, failure and
retirement contracts, and the full workspace build. Independent-queue behavior
is additionally covered by deterministic contracts and earlier native fixtures;
unavailable macOS queue topologies and the separately deferred Windows Vulkan
run remain recorded coverage gaps rather than implied passes.

Transient aliasing now has an explicit value boundary in
`Experimental/RHITransientAliasing.h`. Its creation selectors, DynamicRHI query
and aliased factories, and recorded aliasing barrier remain experimental; the
mixed stable headers include this boundary transitively. The owning
[resource contract](RHIResourceViewsAndTransfers.md#experimental-transient-aliasing)
defines capability fallback, ordering and backing retention. Default production
adoption and performance qualification remain open.

## Change and Promotion Rules

A stable breaking change requires a selected plan or other explicitly reviewed
scope. The implementation change must search and migrate every consumer in the
projects declared by `Durin.dworkspace`, update the owning Runtime documents,
and validate the owning module plus affected project targets. Shared Engine API
migrations complete an `all` build before handoff.

Additive changes are still semantic changes when they alter overload selection,
default behavior, ownership, synchronization, or failure policy. Capability
additions must default conservatively and cannot silently make reserved
vocabulary executable.

Experimental changes do not require a compatibility adapter, but they still
move every workspace consumer atomically and update the owning active plan.
Promotion moves the header from `Experimental/` to the stable root, removes the
experimental qualification from all include sites and documents, and records
the validation evidence that closed the stated gates.

Deprecated stable APIs use a compiler-visible deprecation annotation with a
named replacement and removal milestone. Documentation-only deprecation is
insufficient. Removal follows the same workspace-wide migration rule as any
other stable breaking change.

## Related Documentation

- [RHI command execution](RHICommandExecution.md)
- [RHI resource transitions](RHIResourceTransitions.md)
- [RHI resource views and transfers](RHIResourceViewsAndTransfers.md)
- [Graphics state and bindings](GraphicsStateAndBindings.md)
- [Synchronous compute pipelines](SynchronousComputePipelines.md)
- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [RHI diagnostics and conformance](RHIDiagnosticsAndConformance.md)
- [Render Graph](RenderGraph.md)
- [RDG and RHI multi-queue execution plan](../../Plans/RdgRhiMultiQueueExecution.md)
