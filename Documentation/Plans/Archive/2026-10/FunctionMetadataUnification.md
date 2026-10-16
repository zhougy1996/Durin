# Function Metadata Unification Plan

Summary: Make DFunction a DStructBase metadata object and unify generated property records, construction, and ownership while preserving authored declarations and invocation.

Last reviewed: 2026-10-16

Status: Archived
Completed: 2026-10-16

## Current Status

Completed. Validated implementation revision: `187a6e02a` (Stages 11 and 12).
The completion commit only records acceptance. Stages 11 and 12 implement linked parameter GC schemas, shared native property
validation and detailed linking diagnostics. CoreObjectTests passes 161 tests,
including six new cases for provisional struct schemas, multiple buffers, native
struct collectors, shared storage rejection, nested diagnostic repair and missing
native entries. Full DHT regression passes 324 tests with one existing skip
(`Build/PytestFunctionSchema.log`). The all build and seven reflection-domain
targets pass; editor property/operation pass 63/81 cases, editor asset workflows
pass 40, asset packages pass 190, and all 53 function cases pass in separate
processes with one scheduling slot. Logs use `Build/FunctionSchemaAll.log`,
`Build/FunctionSchemaReflection.log`, `Build/FunctionSchema-<Target>.log` and
`Build/FunctionSchemaIsolated.log`; XML reports use the standard preset folder
and `Build/NativeTestResults/FunctionSchemaIsolated.xml`. Validation used
Windows/MSVC preset `Win64-Debug-DurinEditor`. Runtime contracts, changed-document
validation, all-plan validation and diff inspection are complete. No acceptance
gates remain.
Stages 8 through 10 were accepted at `7201c4c00`.
DFUNCTION declarations, ProcessEvent/typed buffer usage, native aggregate
lifetimes and existing reference policies remain unchanged.

## Goal

Align the main function metadata interfaces and construction model with UE's UFunction/UStruct relationship. Preserve authored DFUNCTION declarations, ProcessEvent dispatch, initialized aggregate parameters, typed buffer access and native thunk behavior. Migrate metadata APIs and every Engine, Sandbox and RoadWeaver consumer without compatibility layers.

## Selected Design

The reference is Epic's [UFunction API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UFunction): struct-derived metadata, property-based return discovery, derived member initialization and one native Invoke entry. Durin retains its synchronous native aggregate and GC contracts; this work does not add a bytecode VM.

- Extract the DType/DStructBase foundations into StructBase.h to break the object/globals/function include cycle. DFunction derives from DStructBase, has reflected intrinsic identity, and is stored at a stable address by its owning class.
- Use DFunction exclusively. Remove FFunction, the detached signature/parameter model, passing enums, and manual signature/property factories; migrate all internal consumers atomically.
- Expose UE-style GetReturnProperty, GetSuperFunction, function flag queries, parameter size/count, native entry, and Invoke. Parameter roles use property flags. Parameter properties and their flags are authoritative. Frames and buffers refer directly to DFunction; generated records supply aggregate layout and lifecycle.
- Share FPropertyParamsBase-derived records and one recursive construction entry between members and function parameters. Typed helpers create records rather than independently instantiate concrete FProperty classes. DHT emits record-backed function descriptors.
- Explicit shared property-tree ownership is common to classes, structs, and functions. Owner back pointers do not own fields. Parents are destroyed before children. Function fields never enter the declaring class's member chain.
- Function overrides keep complete local layouts. SuperStructBase stores parent identity; virtual GetInheritanceSuper selects storage inheritance and returns null for functions. Property enumeration and GC must not duplicate parent parameters.
- Preserve native aggregate construction/destruction and exception cleanup. Preserve strong raw-object function slots, weak/soft non-owning slots, and ordinary member GC rules. No VM, RPC, default-argument, or asynchronous invocation expansion.
- Generated C++ ABI and tool identity change atomically; public export schema changes only if its semantic JSON contract changes.

## Implementation Stages

### Stage 0: Record design and migration boundaries

- [x] Inspect current function metadata, DStructBase, generated property records, and invocation constraints.
- [x] Record selected UE-aligned interfaces, ownership, compatibility, and validation boundaries.
- [x] Validate this plan before implementation.

### Stage 1: Unify property records, construction, and ownership

Depends on Stage 0. Outcome: ordinary fields and detached parameter fields use the same record-driven recursive property constructor and explicit tree ownership.

- [x] Add shared property-tree ownership and attach owned roots to DStructBase.
- [x] Route class/struct and function helpers through the common constructor, including all scalar, enum, object, reference, struct, and container cases.
- [x] Remove the function-only field constructor and container owner builder.
- [x] Validate existing property construction and function value behavior.

### Stage 2: Introduce DFunction metadata objects

Depends on Stage 1. Outcome: DFunction derives from DStructBase, uses stable class-owned registration, and exposes UE-aligned function metadata without changing public invocation behavior.

- [x] Separate metadata foundation headers and introduce intrinsic DFunction identity.
- [x] Migrate function registration, linking, lookup, frames, and consumers to the single object descriptor.
- [x] Attach parameter fields to the function, derive parameter roles/return metadata, and preserve signature compatibility adapters.
- [x] Test reflected identity, ownership, override enumeration, layout, GC, and module/default lifecycle boundaries.

### Stage 3: Unify DHT output and compatibility

Depends on Stage 2. Outcome: generated functions use shared property records and the unified function construction entry.

- [x] Emit record-backed function parameters and return fields with compiler-derived layout/lifecycle.
- [x] Update tool/generated ABI identity and migrate all source/test consumers.
- [x] Keep authored syntax, ProcessEvent and typed buffer usage unchanged; preserve target-compiler signature assertions.
- [x] Run the full DHT test suite and isolated native function tests.

### Stage 4: Integration validation and documentation

Depends on Stage 3. Outcome: all required gates pass and long-lived contracts describe the implemented model.

- [x] Complete an all build covering Engine, Sandbox, and RoadWeaver.
- [x] Run affected native tests, including function/GC, property construction/snapshot, and editor actions; run function cases in isolation.
- [x] Update runtime/tooling documentation, validate changed documents and all plans, and inspect the final diff.
- [x] Record exact results, mark the plan completed only after acceptance, and commit with Plan/Stage provenance.

Validation follows [agent build guidance](../../../Agents/BuildAndRun.md) and [agent testing guidance](../../../Agents/Testing.md). Reports and logs stay under ignored Build/ directories. No application-hosted or GPU execution is required by this metadata refactor.

### Stage 5: Remove compatibility metadata and unify runtime interfaces

Depends on Stage 4. Outcome: a single property-driven DFunction model.

- [x] Derive parameter indexes, return metadata and validation from owned properties.
- [x] Remove legacy signature factories, aliases, duplicate flags/owner accessors and invocation overloads.
- [x] Construct functions through generated records, finalize before registration, and reject invalid metadata during dispatch.
- [x] Preserve aggregate lifetime, override semantics and strong/weak/soft GC behavior.

### Stage 6: Migrate generation and all consumers

Depends on Stage 5. Outcome: Engine, editor, DHT and tests use the new model.

- [x] Migrate every workspace consumer and handwritten test registration.
- [x] Update generated records, generator identity and generator assertions.
- [x] Cover malformed layouts, parameter roles, metadata validity and existing value behavior.

### Stage 7: Validate and document the final model

Depends on Stage 6. Outcome: the compatibility-free model is accepted.

- [x] Complete an all build for every workspace project.
- [x] Pass the reflection domain, editor actions, isolated function cases and full DHT suite.
- [x] Update runtime/tooling contracts, validate documentation and all plans, inspect and commit the final diff with provenance.

### Stage 8: Unify property construction and metadata mutation boundaries

Depends on Stage 7. Outcome: shared checked construction and attachment.

- [x] Add one record-driven property construction/attachment entry for classes, structs and functions, preserving record order and tree ownership.
- [x] Encapsulate layout and property-chain mutation behind checked APIs; provide an explicit construction path for borrowed test fields.
- [x] Guard function-owned field flags and native value operations after successful linking, including nested fields.
- [x] Migrate every workspace consumer without compatibility fields or wrappers.

### Stage 9: Unify super metadata and function linking

Depends on Stage 8. Outcome: function linking finalizes and seals the layout.

- [x] Replace SuperFunction storage with SuperStructBase and add GetInheritanceSuper for layout-specific traversals.
- [x] Move derived parameter metadata validation/initialization from construction into virtual DStructBase::Link/DFunction::Link; separate layout sealing from class dispatch admission and require linked layouts for parameter buffers.
- [x] Publish function admission only after all local overrides validate; preserve late parent registration and failure retry semantics.
- [x] Cover mutation rejection, parent identity, local-only layouts, shared construction order/ownership and failed-link behavior.

### Stage 10: Validate linking and construction integration

Depends on Stage 9. Outcome: migrated APIs and behavior are accepted.

- [x] Complete an all build covering Engine, Sandbox and RoadWeaver.
- [x] Pass reflection-domain, editor property/action, isolated function and full DHT regression selections; broaden coverage for migrated metadata consumers as needed.
- [x] Update runtime/tooling contracts, validate changed documents and all plans, and commit with exact plan/stage provenance.

The mutation boundary seals function metadata after successful linking. Other
metadata retains its existing registration/reassembly lifecycle; this work does
not introduce class hot reload or change native aggregate lifetime/GC policies.

### Stage 11: Cache function parameter reference schemas

Depends on Stage 10. Outcome: buffer GC reuses a linked immutable schema.

- [x] Compile local parameter/return reference operations at function Link and share them across live buffers; remove per-buffer per-GC property compilation.
- [x] Keep this parameter policy separate from ordinary member schemas: direct/container raw object slots are strong, weak/soft slots are non-owning, struct members retain ordinary struct GC semantics and native collectors.
- [x] Preserve provisional struct schema rebuilding and exception/failed-link behavior without stale cached traversal decisions.
- [x] Cover raw/wrapped objects, nested containers, structs, returns, multiple buffers, repeated GC and weak/soft behavior.

### Stage 12: Share property validation and report precise link failures

Depends on Stage 11. Outcome: reusable native storage validation and actionable diagnostics.

- [x] Extract common native property type/storage/capability validation from function-specific checks and reuse it at generated property construction boundaries without rejecting existing supported field kinds.
- [x] Keep function-only roles, supported types, nesting rules and aggregate layout checks in function linking.
- [x] Report class/function identity, nested property path and a concrete failure reason for layout, lifecycle, native storage, parameter-role and override failures; clear diagnostics after successful retries.
- [x] Migrate all consumers/tests; preserve existing declaration/call APIs and aggregate lifetimes.

### Stage 13: Validate schema and diagnostic integration

Depends on Stage 12. Outcome: all acceptance gates pass and contracts are current.

- [x] Complete an all build for Engine, Sandbox and RoadWeaver.
- [x] Pass reflection-domain, editor property/operation, isolated function and full DHT regression selections; include broader property/asset coverage for shared validator changes.
- [x] Update runtime contracts, validate changed documents and all plans, inspect the diff and commit with exact plan/stage provenance.
