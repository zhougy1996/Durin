# Unified RHI Buffers Plan

Summary: Remove logical Storage buffer resources and use ordinary GPU buffers, views, and ordered uploads throughout RHI and its backends.

Last reviewed: 2026-10-09

Status: Completed
Completed: 2026-10-09

## Current Status

Stages 0-3 completed in commit `e9f177cc5`; Stage 4 is implemented and its
available-host acceptance gates passed. Uniform admission, native pages, and
binding resolution now have explicit ownership and separate implementations.
The portable reservation probe and redundant submission lease retention are
removed. Upload commands share recording and payload logic while preserving
canonical versus transfer-write final access.

Stage 4 validation on Windows, preset `Win64-Debug-DurinEditor`, 2026-10-09:

- All ten RHI/RenderCore contract targets passed. Report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/module-rhi-rendercore-kind-contract.xml`.
- All 115 Vulkan integration cases passed on NVIDIA GeForce RTX 3090, including
  captured binding retirement, concurrent reservation, and interval reuse.
  Report: `Build/NativeTestResults/Win64-Debug-DurinEditor/VulkanRHIIntegrationTests.xml`.
- The migrated Uniform reservation case passed alone. Report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/UniformReservationCleanup.xml`.
- The final shared Engine API `all` build passed, including Sandbox and RoadWeaver.
  Receipt: `Build/.agent-state/logs/20261009-203624-412916-51932-cmake.log`.
- Changed-document and all-plan validation, workspace consumer searches, and
  final diff review passed. Runtime ownership contracts are updated.

Earlier migration evidence follows for the Stage 0-3 baseline.

The Stage 0-3 implementation and available-host gates are complete. Logical
Storage resources, CPU shadows, allocation replacement, native-operation
restrictions, and the Storage binding pool are removed. Shader Storage ranges
use ordinary native buffers/views; immutable uniform allocation behavior remains.

Validation on Windows, preset `Win64-Debug-DurinEditor`, 2026-10-09:

- Final `all` build passed, including Engine, Sandbox, and RoadWeaver. Receipt:
  `Build/.agent-state/logs/20261009-202216-258786-18792-cmake.log`.
- All ten targets in `@module=rhi+rendercore,kind=contract` passed. Report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/module-rhi-rendercore-kind-contract.xml`.
- All 115 Vulkan integration cases passed on NVIDIA GeForce RTX 3090. Report:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/VulkanRHIIntegrationTests.xml`.
- The migrated binding case also passed alone, covering inline/threaded,
  graphics/compute, and static/dynamic uniform combinations. It checks distinct
  Storage allocations, preserved partial-upload bytes, and uniform rebinding.
  Report: `Build/NativeTestResults/Win64-Debug-DurinEditor/UnifiedBufferBindings.xml`.
- Changed-document validation, all-plan validation, and diff checks passed.
- Workspace source/test searches found no removed Storage resource symbols,
  API consumers, special allocation paths, or obsolete Runtime descriptions.

GPU validation exposed a legacy compute descriptor check that inferred
read-only access from the old Storage backing path. It now uses
`UnorderedAccess` usage: writable buffers require compute read/write access,
while read-only buffers require compute shader read access. The complete GPU
suite and final build passed after this correction.

Metal source was migrated and reviewed. macOS compilation and GPU execution
were unavailable on this Windows host and are not claimed as passes. They are
platform coverage gaps, not available-host acceptance gates.

The first sandboxed MSVC build failed with C1902 before code diagnostics; the
same authorized build outside the sandbox passed. No compiler flags or build
profile settings were changed. `test affected --explain` expanded to all tests
because of shared runtime inputs; the explicit module contract set and Vulkan
integration suite provide the bounded validation for this change.

## Goal

Use one resource identity (`FRHIBuffer`) for GPU buffers. Structured and
byte-address interpretation belongs to usage flags and views, not a subclass
with different update, synchronization, and binding semantics. Preserve the
independent layout and reference semantics of `FRHIUniformBuffer`.

## Selected Design

- Remove `FRHIStorageBuffer`, its Vulkan/Metal subclasses, the Storage resource
  tag, `CreateStorageBuffer`/`RHICreateStorageBuffer`, and logical Storage binding
  resolution. Do not move the CPU shadow or virtual update function into
  `FRHIBuffer` and do not introduce an equivalent replacement resource class.
- Create GPU buffers with `FRHIBufferCreateDesc` through the existing native
  creation path. Use existing ordered `UploadBuffer`/`WriteBuffer` commands for
  patches; remove the Storage-only `UpdateBuffer` entrypoint.
- Keep shader `StorageBuffer` binding vocabulary and `FRHIStorageBufferRange`:
  they describe shader interpretation and a byte range, not resource identity.
- Native views retain ordinary parent buffers. View factories and caches use
  one native path. Buffer operations and RDG import validate description,
  capabilities, access, and ranges rather than an obsolete resource tag.
- Upload commands own copied source bytes through replay or cancellation.
  Ordinary buffer updates follow GPU write/read dependencies and submission
  order. They do not preserve the contents captured by an earlier binding.
  Callers requiring old contents explicitly allocate a separate buffer and keep
  it alive through the existing submission retirement mechanism.
- Uniform bindings continue capturing immutable uniform allocations. Uniform
  sidecars may retain ordinary buffers/views but must reject uniform resources
  to prevent logical ownership cycles.
- Remove the unused Storage binding arena and accounting where exclusively
  owned by this feature; preserve shared uniform allocation infrastructure.

This follows the separation visible in Epic's public
[FRHIBuffer API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FRHIBuffer),
[usage flags](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/EBufferUsageFlags),
and [buffer view descriptions](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHIViewDesc/FBuffer).
It does not assume identical allocation or update implementations in UE backends.

## Implementation Stages

### Stage 0: Establish the migration contract

- [x] Search Engine, Sandbox, and RoadWeaver source/test consumers.
- [x] Identify the deliberate change from immutable Storage binding snapshots
  to ordinary GPU resource update semantics.
- [x] Record the selected design before implementation.

Acceptance: the removal scope and replacement behavior are concrete and reviewable.

### Stage 1: Unify resources and backend binding paths

- [x] Remove the Storage resource class, creation/update API, resource tag, and
  special-case validation branches across RHI, RenderCore, Vulkan, and Metal.
- [x] Resolve only uniform logical resources; bind Storage through native views.
- [x] Remove Storage-only allocation infrastructure and obsolete includes.

Acceptance: no logical Storage resource implementation or hidden replacement
remains; shader Storage vocabulary continues to work with ordinary buffers.

### Stage 2: Migrate consumers, tests, and lasting contracts

- [x] Migrate all fixtures to ordinary buffers and explicit uploads.
- [x] Replace obsolete rejection/snapshot tests with ordinary-buffer admission,
  range validation, payload ownership, replay ordering, and cancellation checks.
- [x] Cover structured/byte-address views and RDG external-buffer import.
- [x] Migrate GPU correctness coverage to explicit upload dependencies and
  explicit distinct buffers where immutable content is required.
- [x] Update owning Runtime contracts and diagnostics vocabulary.

Acceptance: tests assert the new behavior rather than preserving removed APIs.

### Stage 3: Validate and commit

- [x] Run affected native correctness tests and relevant Vulkan GPU correctness
  coverage on the available Windows host; record unavailable platform coverage.
- [x] Complete the shared Engine API `all` build, including workspace consumers.
- [x] Validate changed documents and all plan lifecycle metadata.
- [x] Review the complete diff, close only evidenced checks, and commit with
  exact Plan and Stage provenance.

Build/test execution follows [Build and Run](../Agents/BuildAndRun.md) and
[Testing](../Agents/Testing.md). Documentation validation follows
[Documentation](../Agents/Documentation.md).

### Stage 4: Simplify uniform and upload internals

Selected design: keep the independent Uniform resource and the separation of
thread-safe reservations from RHI-thread native allocations. Remove generic
Storage-era vocabulary and redundant ownership without changing GPU completion
or public upload final-access semantics.

- [x] Remove `RHIReserveBufferBacking` from portable and public Vulkan RHI;
  expose its only consumer through the existing private Vulkan test interface.
- [x] Name allocation, admission, binding, and resolution paths explicitly for
  Uniform resources and separate pool implementation from binding resolution.
- [x] Replace the one-element reservation vector with a single initialized slot
  and use typed reservations and allocations internally.
- [x] Retain a resolved uniform binding once per submission; its allocation
  transitively retains its reservation. Remove duplicate lease owners and unused
  resolution parameters while preserving captured bindings through completion.
- [x] Validate Uniform layout/lifetime and references directly; correct stale
  Storage accounting comments.
- [x] Share upload command payload, validation, and recording implementation;
  keep `WriteBuffer` canonical access and `UploadBuffer` transfer-write access.
- [x] Migrate all workspace consumers, update lasting contracts, and verify
  cancellation, interval reuse, concurrent admission, and binding retirement.
- [x] Pass relevant RHI/RenderCore contracts, Vulkan integration, the shared
  Engine API `all` build, changed-document and all-plan validators, and diff review.
- [x] Record acceptance evidence and commit with this exact Stage provenance.

Acceptance: no public reservation test hook or generic deferred-buffer resolver
remains; one resolved Uniform binding owns one immutable allocation and its
single reservation; upload entrypoints preserve their documented behavior.
