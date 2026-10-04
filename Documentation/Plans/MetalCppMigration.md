# Metal-cpp Migration Plan

Summary: Migrate the existing MetalRHI backend from Objective-C Metal calls and ARC ownership to Apple Metal-cpp and explicit C++ RAII while preserving observable RHI behavior.

Last reviewed: 2026-10-04

Status: Active
Completed:

## Current Status

Planning only; no refactoring stages below have been completed. Apple Metal-cpp
is already pinned as a dependency. `MetalCppDevice.cpp` performs OS/GPU admission
and creates the device and command queue through C++ APIs, then detaches its
owned references. `MetalDynamicRHI.mm` transfers those references to ARC.
Resources, pipelines, submission, callbacks, and presentation still use
Objective-C Metal calls and ARC-owned objects.

The next implementation step is Stage 0. Existing qualification results are
historical context, not validation of this refactor. Capture a fresh baseline
before changing ownership and compare each subsequent stage against it.

## Goal and Scope

Complete the API and ownership migration for existing MetalRHI functionality.
Keep backend-neutral RHI interfaces, supported capabilities, shader routes,
resource descriptors, synchronization, failure behavior, and rendered/readback
output unchanged. Keep native types private to MetalRHI.

The [Metal RHI plan](MetalRHI.md) owns feature delivery, platform qualification,
shader-route decisions, and production adoption. This independent plan owns
only the refactor; its completion does not close that plan's acceptance gates.
New features, wider hardware support, performance optimization, and changing
the default backend are outside scope.

## Selected Approach

- Migrate complete ownership boundaries in small, independently validated
  commits. Keep `.mm` during intermediate steps; rename or extract to `.cpp`
  only after the relevant code no longer requires Objective-C syntax.
- Use `NS::SharedPtr<T>` for owned native objects and raw `T*` for documented
  borrows. Use `NS::TransferPtr` for an existing owned reference and
  `NS::RetainPtr` when acquiring ownership of a borrowed/autoreleased object.
  Do not use default-deleting `std::shared_ptr` for native Metal objects.
- Temporary ARC/C++ interoperability may retain separate references, but every
  owning reference must have exactly one release responsibility. Centralize
  bridges, name whether they borrow or transfer, and remove temporary bridges
  as their consumers migrate. Never adopt an ordinary borrowed bridge with
  `TransferPtr`.
- Preserve autorelease pools on relevant worker, pipeline compilation, replay,
  and callback scopes. Pool drainage is not a GPU completion signal.
- Convert descriptor construction explicitly against the pinned headers:
  setters, descriptor-array access, enums, defaults, error outputs, and null
  handling. Avoid whole-file mechanical substitutions.
- Keep `NS_PRIVATE_IMPLEMENTATION`, `CA_PRIVATE_IMPLEMENTATION`, and
  `MTL_PRIVATE_IMPLEMENTATION` defined in exactly one translation unit.
- Preserve submission owners through actual GPU use. CPU replay completion,
  native object reference counting, and backing-memory retirement are distinct
  responsibilities, including for buffers backed by externally owned memory.

## Implementation Stages

### Stage 0: Record ownership boundaries and the validation baseline

Dependencies: current MetalRHI source and the prepared pinned dependency.

- [ ] Inventory native fields and creation paths in device/queue, resource,
  pipeline, command, submission, deferred backing, and viewport objects. Record
  each as owned, borrowed, or autoreleased, with its required lifetime.
- [ ] Inspect the pinned headers for smart pointers, completion-handler
  overloads, descriptor arrays, and QuartzCore support. Record necessary
  compiler flags and remaining Objective-C interoperability requirements.
- [ ] Identify existing qualification coverage and run the relevant baseline
  in inline and threaded modes. Record coverage gaps separately from failures.
- [ ] Map the tests needed for pool drainage, delayed completion, readback,
  creation failure, and shutdown with work outstanding to later stages.

Acceptance: an ownership inventory and a reproducible baseline receipt exist;
the first migration has a bounded set of fields, consumers, and tests.

### Stage 1: Move device, queue, and buffer ownership to C++

Dependencies: Stage 0 accepted.

- [ ] Return RAII-owned device/queue values from the creation helper; remove
  the detach-to-ARC handoff and migrate long-lived RHI/submission-state owners.
- [ ] Migrate buffer creation and `FMetalBuffer` storage to Metal-cpp and RAII;
  expose only a borrowed native handle from resource accessors.
- [ ] Adapt existing command consumers with localized, temporary borrowed
  bridges. Audit deferred buffer backing and external storage owners without
  changing submission retirement semantics.
- [ ] Verify initialization failure cleanup, buffer upload/copy/readback,
  release of caller references before GPU completion, and queue shutdown.

Acceptance: devices, queues, and buffers have explicit C++ owners; buffer
outputs match the baseline in both execution modes, with no early release or
new retained-object growth under repeated creation and teardown.

### Stage 2: Migrate samplers, textures, and texture views

Dependencies: Stage 1 accepted.

- [ ] Migrate sampler creation and ownership, then textures and texture views,
  including viewport backing textures and temporary transfer resources.
- [ ] Isolate descriptor translation in focused helpers and preserve support
  checks, allocation reporting, null results, and recoverable creation errors.
- [ ] Verify currently supported dimensions, mip/layer/subresource operations,
  uploads, copies, views, and readbacks against existing coverage.
- [ ] Exercise view/source lifetime and pool drainage while GPU work remains
  outstanding; retain any owner required by the existing RHI contract.

Acceptance: resource wrappers and creation paths use Metal-cpp; supported
transfer outputs and unsupported-descriptor behavior match the baseline.

### Stage 3: Migrate shaders and pipeline construction

Dependencies: Stage 2 accepted.

- [ ] Migrate library/function and compute pipeline ownership and creation.
- [ ] Migrate graphics pipeline, vertex layout, and depth/stencil descriptors
  field by field, preserving binding maps and error diagnostics.
- [ ] Preserve autorelease pools for asynchronous pipeline work and ensure
  diagnostic text outlives any borrowed native error object when necessary.
- [ ] Verify successful and failed creation, asynchronous compilation/cache
  teardown, pipeline replacement, and existing representative draw/dispatch
  outputs without changing shader generation or artifact identity.

Acceptance: pipeline creation and stored native state use C++ RAII; existing
draw/compute results and complete-or-failure publication behavior are preserved.

### Stage 4: Migrate command encoding and submission lifetime

Dependencies: Stage 3 accepted.

- [ ] Migrate command buffers, render/compute/blit encoders, render-pass
  descriptors, and draw/dispatch/copy operations through bounded substeps.
- [ ] Replace native resource arrays with owning C++ containers while retaining
  replay storage, RHI resource owners, deferred backing, and readback buffers.
- [ ] Move completion handling to the pinned C++ API with an independently
  owned completion payload. Avoid capturing a destructible raw backend pointer
  or forming a command-buffer/callback ownership cycle.
- [ ] Preserve timeline publication order, completion notification, failure,
  cancellation, timeout, and shutdown behavior. Audit all completion handlers,
  including transfer and presentation paths, rather than only normal submit.
- [ ] Exercise empty submissions, delayed completion, pool drainage, upload
  reuse, readback success/failure, and teardown with outstanding callbacks in
  inline and threaded modes. Label simulated failure evidence explicitly.

Acceptance: CPU replay cannot release storage still needed by GPU work;
readbacks complete only from valid GPU completion, terminal paths release their
owners, and the existing submission contract remains intact.

### Stage 5: Isolate Cocoa interoperability and finish C++ extraction

Dependencies: Stage 4 accepted.

- [ ] Migrate supported layer/drawable operations through Metal-cpp QuartzCore
  wrappers; retain a small `.mm` adapter for necessary Cocoa window integration,
  native object validation, and explicit ownership bridges.
- [ ] Verify drawable acquisition failure, present completion, resizing,
  repeated viewport creation/destruction, and shutdown. Keep window ownership
  with its existing platform owner.
- [ ] Extract the native C++ implementation into cohesive private files and
  update module/CMake source registration. Restrict ARC flags to remaining
  Objective-C++ files and remove obsolete temporary bridges and imports.
- [ ] Run the full affected Metal qualification set and relevant project
  builds; inspect the remaining Objective-C usage and document its purpose.
- [ ] Move lasting ownership/interop rules into the owning implementation
  documentation and record final evidence and remaining qualification gaps.

Acceptance: core MetalRHI resources, pipelines, commands, and submission compile
as C++; Objective-C++ is limited to justified Cocoa interoperability. Existing
headless and available presentation workloads retain their baseline behavior.
Unavailable presentation evidence remains an open gate, not a passing result.

## Validation and Handoff

Follow [build and run guidance](../Agents/BuildAndRun.md),
[native-test guidance](../Agents/Testing.md), and the
[documentation workflow](../Agents/Documentation.md). Use the host DevTool
launcher and existing qualification targets; do not copy stale command lines
into this plan. Do not overlap build process trees.

For each stage, record revision, pinned dependency version, SDK/compiler,
OS/GPU, execution modes, workloads, report paths, output comparisons, and
coverage gaps. Compilation alone does not establish ownership correctness.
Use available Metal validation and lifetime diagnostics for repeated teardown
and outstanding-work cases. Keep unimplemented or unverified checks open.

Each implementation commit updates this plan's status/checklists and uses this
file and the exact implemented stage title for `Plan` and `Stage` trailers.
Prefer backend-private changes. If a shared API must change, migrate all
consumers in `Durin.dworkspace` and complete the required `all` build before
handoff, as required by the repository rules.

## Required References

- [RHI command execution](../Runtime/Rendering/RHICommandExecution.md)
- [RHI resource transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI resource views and transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [RHI diagnostics and conformance](../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [Graphics state and bindings](../Runtime/Rendering/GraphicsStateAndBindings.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)
- [Apple Metal-cpp ownership and interoperability guidance](https://developer.apple.com/videos/play/wwdc2022/10160/)

The pinned local headers remain authoritative for API availability in this
refactor; upstream documentation does not justify a dependency upgrade.
