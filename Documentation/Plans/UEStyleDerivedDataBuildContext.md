# UE-Style Derived Data Build Context Plan

Summary: Replace Durin's returned-output and typed-failure build-function contract with UE-style context-owned inputs, outputs, deterministic messages, Compact Binary constants, and fixed value identifiers.

Last reviewed: 2026-09-29

Status: Active
Completed:

## Current Status

No implementation has started. The completed
[Derived Data Build Interface Alignment](DerivedDataBuildInterfaceAlignment.md)
plan established `IBuild`, persistent sessions, immutable actions and outputs,
and the current local cache execution contract. It deliberately retained
`std::expected`, constrained constant variants, string value identifiers, and a
public `IBuildFunction::Validate` hook, while excluding Compact Binary.

This plan changes those decisions. It selects the UE public responsibility
model for function execution: a function reads constants and inputs from
`FBuildContext`, writes values, metadata, and deterministic messages back to the
context, and returns no product or public failure object. It also introduces a
Durin-owned Compact Binary implementation in Core and a 12-byte `FValueId` in
DerivedDataCache. It does not claim source or wire compatibility with Unreal
Engine and does not copy UE implementation code.

The first stage must freeze cache compatibility, deterministic error caching,
and the replacement for family semantic validation before shared APIs change.
Those decisions affect every producer and cannot be deferred into migration.

## Goal

Make `FBuildContext` the only build-function boundary for deterministic inputs
and outputs. A registered function is stateless, returns `void`, and reports a
deterministic build failure with `AddError`. The build executor owns request,
infrastructure, cancellation, and cache-recovery status. Successful output is an
immutable collection of `FValueWithId`, metadata, and messages; output with an
error contains no values.

Replace public construction from vectors and mutable output data with builders
created by `IBuild`. Constants use immutable Compact Binary objects, output
values use fixed 12-byte identifiers, and input keys remain UTF-8 names. Preserve
the existing local-only scheduler, captured-input warm-hit behavior, owner
publication rules, and zero-copy retained buffers.

## UE Reference and Selected Scope

The following Epic public interfaces define the selected responsibility model:

- [`IBuildFunction`](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/IBuildFunction)
  is stateless, receives inputs through `FBuildContext`, saves outputs through
  the context, and has `void Build(FBuildContext&) const`.
- [`FBuildContext`](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildContext)
  exposes `FindConstant`, `FindInput`, `AddValue`, `AddMeta`, deterministic
  messages, and errors.
- [`FBuildDefinitionBuilder`](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildDefinitionBuilder)
  adds uniquely named Compact Binary constants and input references before
  freezing an immutable definition.
- [`FValueId`](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FValueId)
  is a 12-byte context-local value identifier with name/hash construction and
  indexed derivation.
- [`FBuildOutput`](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildOutput)
  is immutable, contains values, metadata, messages, and logs, and contains no
  values when it has an error.

Durin adopts those responsibilities without adding remote execution, worker
export, transitive build graphs, `IRequestOwner`, request barriers, per-value
cache policies, or asynchronous build functions. `BeginAsyncBuild`,
`EndAsyncBuild`, `CancelAsyncBuild`, `FCompositeBuffer`, and remote build status
flags remain out of scope until a production requirement justifies them.

## Selected Architecture

### Compact Binary ownership

Core owns a bounded immutable Compact Binary value system because
DerivedDataCache may depend only on Core. The initial public surface includes
owned and view forms for fields, objects, and arrays; a writer; validation; and
canonical serialization. It supports null, bool, signed and unsigned integers,
32/64-bit floating point, strings, binary attachments or byte strings, object
IDs, arrays, and objects. Exact supported kinds and names are frozen in Stage 0.

Canonical encoding defines byte order, numeric widths, field ordering, duplicate
field rejection, maximum nesting, maximum field count, maximum string length,
and maximum encoded bytes. Owned objects retain immutable shared storage; views
cannot outlive their owner. Hashing never depends on C++ layout, unordered
container iteration, locale, or host endianness.

Durin may use UE vocabulary such as `FCbObject`, `FCbObjectView`, `FCbField`,
`FCbFieldView`, and `FCbWriter` only when the semantics match the documented
responsibility. No binary compatibility with UE Compact Binary is assumed
unless separately specified and tested.

### Definition, action, and builders

`IBuild` creates move-only builders for definitions, actions, inputs, and
outputs. Builders reject duplicate keys and invalid values before producing an
immutable handle. Public callers no longer construct `std::vector<FBuildConstant>`
or `FBuildOutputData` directly.

`FBuildDefinitionBuilder::AddConstant(Key, FCbObject)` records uniquely named
constant objects. `FBuildDefinition` and `FBuildAction` expose lookup and ordered
iteration without exposing storage. Action identity hashes an explicitly
versioned canonical encoding of the registered function, constants, and resolved
input identities.

Changing from the current variant encoding is an intentional identity change.
The new action encoding uses schema 3 unless Stage 0 proves and selects a
byte-identical compatibility encoder. Family `ConstantsSchema`, function
versions, and output schemas change when their interpretation or output contract
changes; version bumps are not used as a substitute for an envelope version.

### Input and output identities

Input references and materialized input blocks retain UTF-8 names because build
functions call `FindInput(Key)`. Output and cache-record values use `FValueId`.
Do not reuse one `FBuildValue` type for both roles.

`FValueId` owns exactly 12 bytes, reserves the all-zero value as null, supports
stable comparison and hashing, and provides `FromName`, `FromHash`, and
`MakeIndexed`. Stage 0 freezes the derivation algorithm, byte order, index range,
and multidimensional index packing before any persisted ID is produced.

The output model separates identity from content:

- `FValueId` identifies one slot within an output or cache record.
- `FValue` identifies content by raw hash and raw size and may retain data.
- `FValueWithId` combines the slot ID with the value.
- `FBuildValueKey` combines an action/build key with a value ID when a stable
  cross-output reference is required.

Texture mip, cube face/mip, volume mip, StaticMesh LOD/stream, collision block,
and shader entry/code/reflection IDs are declared as stable family constants;
dynamic strings such as `Mip/0` and `LOD/0/Positions` leave persisted output.

### Build context and output construction

`IBuildFunction` becomes:

```cpp
class IBuildFunction
{
public:
    virtual ~IBuildFunction() = default;
    virtual auto GetName() const -> std::string_view = 0;
    virtual auto GetVersion() const -> FBuildVersion = 0;
    virtual auto Configure(FBuildConfigContext& Context) const -> void;
    virtual auto Build(FBuildContext& Context) const -> void = 0;
};
```

`FBuildContext` provides bounded lookup for constants and inputs and bounded
output mutation through `AddValue`, `AddMeta`, `AddMessage`, `AddWarning`, and
`AddError`. It also exposes cancellation and required-memory facts. The context
is single-threaded during synchronous `Build`; all mutation becomes invalid when
the function returns. The executor alone freezes the output builder.

Duplicate/null value IDs, duplicate metadata keys, invalid Compact Binary,
message overflow, value-count overflow, and byte-budget overflow cause a
controlled build error. If any error message exists, freezing discards all
values and produces an error output. A successful output must meet its declared
family contract before persistence or completion.

### Error and completion model

Remove public `FBuildFailure`, `FBuildFunctionResult`, producer codes,
diagnostic identities, and operation-tagged failure values from build-function
and completion APIs.

Errors separate by ownership:

- Deterministic errors derived only from constants and inputs use
  `FBuildContext::AddError` and become `FBuildOutputMessage` entries.
- Warnings and informational deterministic diagnostics use `AddWarning` and
  `AddMessage`.
- Cache, dispatch, allocation, exception, and other environmental failures are
  executor diagnostics and complete with `EStatus::Error`; they never become a
  cacheable deterministic build message.
- Cancellation completes with `EStatus::Canceled` and is never represented as
  an error message.

`FBuildCompleteParams` exposes `Status`, `BuildStatus`, `CacheKey`, and
`FBuildOutput`. An error output preserves its deterministic messages but owns no
values. Infrastructure failure may have an empty output and retains bounded
internal trace diagnostics rather than a public typed failure. Consumers must
not parse message text to make publication decisions.

Stage 0 decides whether deterministic error outputs are persisted as negative
cache entries. If enabled, their identity, message ordering, localization rules,
expiry behavior, and invalidation by function version must be deterministic and
tested. Environmental failures are never persisted.

### Validation and cache recovery

The existing public `IBuildFunction::Validate` and validation-receipt contract
does not match the selected UE function surface. Stage 0 must select and record
one replacement before implementation:

1. make builder/load validation and content hashes sufficient for trusted
   `FBuildOutput`, with typed family assembly owning only product construction;
2. retain family semantic validation behind an executor-private registry hook;
3. move family validation into a shared output codec used identically by local
   output freeze and cached output load.

The selected path must preserve corrupt-cache fallback, prevent invalid fresh
output from persistence, avoid duplicate full scans where receipts currently do
so, and keep typed family products outside DerivedDataCache. A cache decode or
integrity failure remains recoverable when policy allows local build.

### Compatibility and non-goals

Compact Binary constants, `FValueId`, and context-owned error output change
action identity and cache-record encoding. Prefer explicit cold invalidation over
an indefinite compatibility facade. If legacy reading is retained, it is a
bounded schema-1-to-schema-2 cache-record decoder only; no legacy writer remains.

Authored package bytes, Cook payload bytes, runtime asset codecs, family source
identity, scheduling, latest-wins publication, shader single-flight/LRU, and
GPU/runtime readiness semantics do not change. No UE binary/source compatibility
claim, remote execution, new worker process, new thread pool, or global static
function registration is introduced.

## Implementation Stages

### Stage 0: Freeze wire, validation, and error decisions

- [ ] Inventory every public and private consumer of `FBuildFailure`,
  `FBuildOutputData`, `FBuildValue`, `IBuildFunction::Build`,
  `IBuildFunction::Validate`, validation receipts, and string value IDs across
  all projects declared by `Durin.dworkspace`.
- [ ] Freeze the Compact Binary type set, canonical encoding, ownership model,
  limits, and malformed-input behavior.
- [ ] Freeze `FValueId` derivation, indexing, comparison, serialization, and all
  family ID namespaces, including multidimensional index packing.
- [ ] Select the semantic-validation replacement and prove how corrupt cached
  output falls back without restoring a public typed failure.
- [ ] Decide deterministic error-output caching and document the exact status,
  output, and retry behavior for cache hits and local builds.
- [ ] Record the action, cache-record, function, constants, and output schema
  version changes and whether any bounded legacy reader exists.
- [ ] Capture current canonical action bytes, record bytes, output sizes, cold
  and warm timings, source-read counts, and retained-buffer behavior as the
  migration baseline.

Depends on no later stage. Complete when no wire, validation, error, or
compatibility choice remains implicit.

### Stage 1: Add bounded Compact Binary to Core

- [ ] Implement immutable owned/view field, object, and array types with a
  canonical writer and bounded validator in Core.
- [ ] Enforce canonical numeric widths, byte order, object field ordering,
  duplicate rejection, depth/count/byte limits, and owner-retaining views.
- [ ] Add golden byte, round-trip, malformed input, boundary, ownership,
  endianness, float, ordering, and allocation-failure tests.
- [ ] Document the Compact Binary contract in the owning Core serialization
  documentation without presenting this plan as the long-lived specification.

Depends on Stage 0. Complete when Core tests prove deterministic bounded
encoding and DerivedDataCache can consume the API without a reverse dependency.

### Stage 2: Introduce value identities and the new cache value model

- [ ] Add `FValueId`, `FValue`, `FValueWithId`, and `FBuildValueKey` with null,
  comparison, hash, name/hash construction, and indexed-ID tests.
- [ ] Split named input blocks from ID-addressed output values.
- [ ] Change `FBuildOutput` lookup and ordering to `FValueId`; add stable family
  ID declarations and collision/duplicate tests.
- [ ] Update cache records to the selected envelope schema, preserving content
  hashes, optional retained data, limits, raw/compressed round trips, and corrupt
  record rejection.
- [ ] Measure descriptor size and lookup behavior against current string IDs.

Depends on Stages 0-1. Complete when all values round-trip through raw and
compressed records with fixed IDs and no producer still persists a string value
identifier.

### Stage 3: Add UE-style immutable builders and service factories

- [ ] Add move-only definition, action, inputs, and output builders created by
  `IBuild` and backed by private immutable state.
- [ ] Replace public vector construction of constants and outputs with builder
  methods that reject invalid or duplicate entries at insertion/freeze.
- [ ] Store constants as Compact Binary objects and expose lookup/ordered
  iteration without leaking the backing container.
- [ ] Encode action schema 3 and add golden identity tests proving insertion
  order independence and type/value sensitivity.
- [ ] Keep debug names out of identity and bound every builder-owned table and
  byte allocation.

Depends on Stages 1-2. Complete when callers cannot construct a valid definition,
action, inputs container, or output except through its validated builder path.

### Stage 4: Move build results and deterministic errors into context

- [ ] Change `IBuildFunction::Build` to `void Build(FBuildContext&) const` and
  give Context `FindConstant`, `FindInput`, `AddValue`, `AddMeta`, message, error,
  cancellation, and resource-query APIs.
- [ ] Freeze output after synchronous function return and enforce that error
  output has messages but no values.
- [ ] Remove public typed producer failure from completion; keep bounded
  executor-only diagnostics for infrastructure and recovery analysis.
- [ ] Implement the selected validation and deterministic error-cache behavior.
- [ ] Test duplicate output IDs, writes after return, error-after-value,
  cancellation, exceptions, corrupt cache fallback, deterministic message
  ordering, and exactly-once completion.

Depends on Stage 3. Complete when a synthetic build function uses only Context
to read all deterministic inputs and publish every deterministic result.

### Stage 5: Migrate all producer families and consumers

- [ ] Migrate Texture2D, TextureCube, and VolumeTexture to Compact Binary
  constants, named input lookup, fixed output IDs, Context output writes, and
  deterministic messages.
- [ ] Migrate StaticMesh render and physics collision while preserving source
  capture, reconciliation, working-set bounds, typed assembly, and Cook bytes.
- [ ] Migrate ShaderBuild while preserving dependency closure verification,
  compiler scheduling, single-flight, LRU, cancellation, and compiler messages.
- [ ] Replace family mappings from `FBuildFailure` with status/output-message
  handling that never parses message text for correctness.
- [ ] Preserve one persistent session per owning service and verify warm hits do
  not resolve source payloads.

Depends on Stage 4. Complete when every production family uses only the new
Context contract and all publication gates remain owner-controlled.

### Stage 6: Remove legacy surfaces and qualify compatibility

- [ ] Delete `FBuildConstantValue`, public `FBuildConstant`, `FBuildOutputData`,
  string output IDs, `FBuildFailure`, `FBuildFunctionResult`, public
  `IBuildFunction::Validate`, and superseded validation-receipt plumbing after
  all consumers migrate.
- [ ] Search every declared project for direct mutable output construction,
  typed generic failure inspection, legacy action schema assumptions, and
  string output value IDs.
- [ ] Verify package/Cook byte identity, family output behavior, cache miss/hit,
  force-build, error output, corruption recovery, cancellation, reload, drain,
  and shutdown behavior.
- [ ] Run affected native-test batches and the required shared Engine API `all`
  build using the repository build and testing workflows.
- [ ] Run CPU qualification for texture, StaticMesh, physics, and ShaderBuild;
  investigate action-size, record-size, cold/warm time, memory, source-read, and
  retained-buffer regressions before closing the stage.

Depends on Stage 5. Complete when no legacy producer/output/error surface remains
and all changed project targets pass.

### Stage 7: Publish the implemented contract

- [ ] Update the Derived Data Build Protocol and affected family contracts with
  the implemented builders, context, errors, value IDs, cache compatibility, and
  validation ownership.
- [ ] Record exact validation commands, target counts, qualification results,
  platform coverage, compatibility decisions, and intentional deferrals in this
  plan.
- [ ] Run changed-document and all-plan validation and close only evidence-backed
  checklists.
- [ ] Mark the plan completed only after lasting rules live in their owning
  Runtime, Development, and Core documentation.

Depends on Stage 6. Complete when documentation and validation describe the
implemented contract without relying on this active plan as runtime authority.

## Acceptance Gates

- [ ] A build function returns `void`, reads constants/inputs only through
  `FBuildContext`, and publishes values/messages only through Context.
- [ ] Deterministic build errors are output messages, error output contains no
  values, infrastructure failure is not cacheable output, and cancellation is a
  distinct terminal status.
- [ ] Constants are immutable Compact Binary objects with bounded canonical
  encoding; no public constrained-variant constant representation remains.
- [ ] Output values use stable 12-byte IDs, inputs retain names, and no persisted
  output value uses a dynamic string ID.
- [ ] Action and cache-record schema transitions are explicit, tested, and do
  not silently reinterpret legacy bytes.
- [ ] Corrupt cache data falls back according to policy, invalid fresh output is
  never persisted, and the selected validation boundary performs no unsafe
  trusted bypass.
- [ ] Texture, StaticMesh, physics, and Shader behavior, package/Cook bytes,
  publication checks, warm source avoidance, and retained-buffer properties are
  preserved.
- [ ] Affected tests, CPU qualifications, shared API `all` build, documentation
  validation, and exact legacy-symbol searches pass on the selected host.

