# RHI Public API Stability

Summary: Define the source and semantic stability boundary of public RHI headers without promising a C++ binary ABI or unsupported GPU features.

Modules: RHI, RenderCore, VulkanRHI

Last reviewed: 2026-09-29

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
- reflected shader parameters, descriptor arrays, and push constants;
- viewport presentation, initialization, capabilities, diagnostics, and
  renderer-facing pipeline-creation requests; and
- queue-qualified GPU completion, explicit queue ownership transfer, and the
  related `FDynamicRHI`, command-context, RDG allocation, and submission
  recording operations.

The owning semantic documents remain authoritative. This document classifies
their API surface and does not duplicate their detailed rules.

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

Transient aliasing is not a public experimental contract until its owning stage
introduces a complete header and behavior boundary.

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
