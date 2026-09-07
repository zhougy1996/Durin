# Asset Registry and Cook Consistency Plan

Summary: Separate owned Registry queries from live admission and make Cook reuse depend on explicit, consistently captured build inputs.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Investigation confirmed that `FAssetRegistrySnapshot::ResolveAssetPath` reads
current global projection fences while retaining the captured catalog revision.
Fence insertion and removal do not advance that revision. The shared resolver
also consults the global class registry. The existing redirect-hop fence test
actually expects the snapshot query to change when a live fence changes.

Cook captures Registry metadata but also inspects live package files, captures
external reference stores, queries loaded classes, hashes source files, and
loads resident objects. Its dependency fingerprint currently incorporates
Registry reference fingerprints, which are metadata projections rather than
proof of dependency package payload identity. Fixing only the snapshot resolver
does not establish a consistent Cook input set.

Stage 0 source inventory is recorded below against `14e7ccedd`. The selected
capture protocol requires Engine loader, bulk-resource, callback-lifetime, and
ShaderBuild integration; it cannot be implemented solely in AssetRegistry or
CookCoordinator. No complete implementation stage has passed its acceptance gate.
The protocol review checklist below remains an implementation entry gate.
An initial source change now separates external-root capture from reachability:
Cook passes one owned capture, and capture rejects reentrant registration
changes before invoking another provider. This is preparatory work, not a
replacement for the full input protocol. The affected build passed; 78 of 79
affected test targets passed, including AssetPackageTests, AssetCookTests,
AssetReferenceStoreTests, and PackageRegistryContractTests. The remaining
VulkanRHIIntegrationTests target passed all 68 cases on an exact rerun, without
source changes. The initial aggregate failure remains recorded. Stage 0 remains open for
the loader/resource and run-wide provider-lifetime integration design.

Bulk capture now has a concrete owned resource entry point:
`CreateOwnedPackageResource` copies and validates the exact segment that later
lazy reads consume, without global registration or filesystem access. Regression
coverage exercises caller-buffer mutation/release, unload/reload, bounded reads,
retirement, and rejection of mismatched segment and field identities. The Cook
loader does not yet use this entry point; recursive loading, capture-local
rollback, and provider retirement remain open Stage 0 integration gates.
Validation for this resource entry point: the affected build passed and 77/79
targets passed, including AssetBulkContainerTests, AssetPackageTests, and
AssetCookTests. VolumetricCloudSceneVulkanTests failed its compile-budget check
in the aggregate but passed on an exact rerun. VulkanRHIIntegrationTests crashed
in both the aggregate and exact rerun (Windows access violation, 3221225477);
the failure remains unresolved. No Vulkan source or test policy was changed.

The v9 load context now passes an explicit bulk resource through the linker to
field decoding. Ordinary loose loading supplies its registration result;
captured callers can supply an owned resource with no global registration.
The targeted regression removes the source package and companion, verifies
missing-resource failure, and loads from owned bytes while the skeleton callback
releases the caller's handles. Delayed field reads and retained result buffers
remain valid. Recursive object loading, rollback ownership, and run-wide provider
lifetime are still separate unresolved gates; this is not a sealed Cook session.
Validation for explicit resource propagation passed: the targeted owned-bulk
load regression and all 79 affected native test targets, including ordinary
package, Cook, cooked mesh, texture Cook, and Vulkan integration coverage.
Earlier Vulkan failures above remain historical evidence; this pass does not
identify or fix their cause.

An explicit v9 dependency-load policy now routes both linker package resolution
and external object-field resolution through retained callbacks, with a required
invocation-owned rollback callback. Partial policies fail before skeleton
creation; explicit policy failures do not fall back to live loading or global
snapshot cleanup. The targeted regression covers resident-target rejection,
typed package/object failures, unrelated residency surviving rollback, and
caller-side policy release during skeleton publication. Arbitrary constructor
and `PostLoad` loads still require capture-session enforcement, and provider
code lifetimes remain a caller contract. Stage 0 is not complete.
Validation passed for the policy seam: its targeted regression and all 79
affected native targets. The run was ordinary correctness coverage; another
checkout was active, so it supplies no exclusive-lane performance evidence.

The explicit policy can now reject synchronous implicit live loads with
`bRejectImplicitLiveLoads`. Engine checks package/object and non-null soft-object
entry points before resident or file access. A rejection is retained by all
enclosing guards, forcing graph rollback even if a constructor or `PostLoad`
ignores it. A targeted regression covers all four entry points in both phases
and successful ordinary loading after scope exit. Direct object lookup, raw
file I/O, asynchronous work, and provider lifetime still require the complete
capture protocol; the guard is not an input session and Cook has not enabled it.
Validation passed for the guard: the targeted constructor/PostLoad regression
and all 79 affected native targets, including package and Cook coverage.

The four loader/resource preparatory commits were rebased onto `ed2b83b35`
and consolidated. Conflict resolution retains the prepared-package graph path
and routes ordinary graph bulk/object reads through its shared bindings, while
preserving explicit policy error codes and constructor/PostLoad guard checks.
The affected build and all 80 affected native test targets passed after the
merge, including the upstream object-replacement coverage. Stage 0 remains open.
A preliminary source/test draft
was preserved in the ignored local file
`Documentation/Local/AssetRegistryCookRefactor.patch`; all eleven draft source
and test files were restored before this plan was committed. The draft is not
an implementation baseline and is not required to execute this plan. In
particular, its before/after digest checks are not an input-isolation protocol.
Do not apply it wholesale or treat its proposed APIs as accepted contracts.

## Goal

For an unchanged owned Registry view and explicit query arguments, resolution
returns the same result regardless of current fences, publications, loaded
objects, or type registration. Runtime operations retain current-state safety
checks. Cook fingerprints and captured outputs describe the same input
generation, and incremental reuse responds to package-specific build inputs
rather than unrelated global Registry publications.

Adopt UE's separation of live Registry services, independent Registry state,
and package build dependencies. Do not copy UE implementation code or claim
compatibility with UE APIs. Existing Durin package ownership, recovery,
manifest-last publication, and typed failure contracts remain authoritative.

## Selected Decisions

- AssetRegistry owns metadata values, pure path/redirect queries, publication
  conflict detection, and projection admission facts. Engine owns loaded class
  validation, object residency, package-byte access, Cook dependency collection,
  and the operation protocol that spans checking and using those facts.
- Keep the process-local catalog revision as a publication concurrency token.
  It is not a persistent content identity, a fence version, or a read lease.
  A global revision mismatch alone must not invalidate unrelated Cook inputs.
- Snapshot queries have no implicit fence, global type, UObject, or filesystem
  access. Class compatibility belongs in a separately named operation check or
  an explicit fixed type view. Preserve exact object paths and descendant
  suffixes as well as package queries.
- Live resolution/admission checks every traversed alias and final package.
  Historical snapshot success does not authorize reading current artifacts.
  A point-in-time validation function must not advertise lease semantics.
- Cook build inputs use stable logical names and canonical content values.
  Keep the repository's existing XXH3-128 content identity unless a separately
  justified integrity requirement needs another algorithm. Hashes do not
  provide locking, input completeness, or cross-store atomicity.
- Separate runtime reachability from build invalidation. A package may affect
  a build without belonging in runtime output. Represent direct and transitive
  build dependencies explicitly; do not silently redefine every soft runtime
  reference as a transitive build dependency.
- Persist dependency identities/values with each package's Cook state. Include
  source and owned bulk inputs, relevant target/settings, contributor/producer
  versions, and explicitly declared external/config/schema inputs. Canonical
  encoding must reject ambiguous or duplicate identities and bound decoding.
- An incompatible tool-only Cook-state format becomes a cache miss and is
  regenerated. Authored packages and shipped runtime formats need no migration
  solely for this refactor. Record version policy when implementing the format.
- Detach validated save plans before opening the existing output transaction.
  Invalid, fenced, changed, or unavailable inputs fail with typed outcomes;
  preserve the prior output generation. Any retry must recapture all inputs
  whose validity was lost and be bounded and cancellable.

## Stage 0 Protocol Record

This section selects the implementation contract; it does not describe safety
already supplied by the current APIs. Source observations are from `14e7ccedd`.

### Participating owners and current protection

| Participant | Observed implementation and lifetime | Required capture boundary |
| --- | --- | --- |
| Catalog, references, projection fences | `AssetRegistryState.cpp` uses one shared mutex for owned values. Publication and fence changes take its exclusive side. Snapshot capture releases it before returning. | Copy metadata and admission facts together under a short Registry lock; never keep that lock across Engine calls. |
| Scan and mounts | `AssetRegistryScan.cpp` enumerates current mount paths and files before publication. Incremental scan can reuse size/time-qualified headers. | Capture mount resolution once. Read and validate actual selected package bytes; a scan cache entry is not content evidence. |
| Save, relocation, fix-up, removal, recovery | `AssetPackageOperations.cpp` stages package/companion bytes and publishes projection afterward. Mutation services share loader state without a common read-session mutex. Removal includes externally executed deletion. | Prevent participant mutation during artifact acquisition on the object owner thread. Once detached, later source writes cannot change the captured bytes. |
| Loading and object residency | `AssetRuntime.cpp` returns resident packages before reading disk; fresh load registers loose bulk resources. `FAssetLoadService` has ordinary sets and counters, not concurrent synchronization. | Use a capture-specific loader route for every recursive load and retain capture-owned package roots until all consumers finish. Never fall back to live residency. |
| Bulk and source bytes | `PackageResource.h` offers range reads and retirement with local mutexes. Loose registration still refers to physical storage. `FAssetPackageReadContext` already accepts package and bulk byte views. | Decode from owned package/companion buffers or a resource that owns those buffers. Every later bulk range must refer to that generation. |
| Object lifetime and compilation | `ObjectLifecycle.cpp` checks GameThread when initialized. `AssetCompilingManager.cpp` uses GameThread entry checks and releases its mutex before manager calls. Engine contributors can finish compilation and thereby apply pending source changes. | Settle pending authored changes before capture begins; allow only preparation of capture-owned derived products afterward. Do not hold Registry, resource, or contributor mutexes while joining work. |
| Reflected classes and schemas | `CoreDObject/Private/DObject/Class.cpp` declares reflected types process-lifetime and stores raw pointers in unsynchronized maps. | Run after registration on the object owner thread; freeze registration for the run. Persist stable schema/version values, never pointer identities. Module code must outlive all callbacks and captured objects. |
| External reference stores | At the inventory baseline, `AssetMutationRegistry.cpp` stored raw provider pointers and invoked providers while iterating that map. Preparatory capture now copies the list and rejects registration changes after each callback. Unregister still does not wait for readers. | Retain the owner-thread callback lifetime contract until owner pins are integrated, then retain only owned store snapshots. Reentrant unregister must not invalidate iteration or destroy an active provider. |
| Cook contributors | `CookCoordinator.cpp` copies registrations under its own mutex, then invokes them unlocked. Copying `std::function` does not pin native module code or captured raw pointers. | Acquire a run registration snapshot with an explicit owner lifetime. Retiring a registration prevents new runs; active run tokens delay destruction. |
| Shader auxiliary output | `RenderCore/Private/Shader/ShaderData.cpp` invokes a modular feature per call. `ShaderLibraryProducer.cpp` freezes runtime inventory but then compiles mounted sources. | Retain one provider invocation lifetime and fixed inventory, compiler identity, options, and include/source artifacts through library production. Per-call provider lookup or a source manifest alone is insufficient. |

There is no existing global lock order that can protect all these participants.
The selected implementation must not add a recursive mutex around `Run` and
claim that it synchronizes writers which never acquire it. Registry and
contributor locks are copy-only locks, acquired separately and released before
callbacks, file I/O, object construction, compilation waits, or destruction.
Package-resource locks retain their existing local ordering; no Cook lock is
held while waiting for resource requests. Object work remains serial on
GameThread, or on the single bootstrap owner thread before GameThread setup.
Concurrent raw object/property access is outside that existing thread contract.

### Selected execution protocol

Use **fixed input artifacts with protected object capture**, not a read lease
over live files. A move-only Engine-owned `FCookInputCapture` progresses through
`Acquiring`, `Sealed`, `Capturing`, and `Detached`, or terminal `Failed` /
`Cancelled`. These names are proposed API contracts for the following stages.

1. Validate owner-thread execution and reject nested Cook. Settle pending
   authored compilation before acquiring inputs. Capture project settings,
   mounts, contributor registrations, schema identities, shader provider, and
   external root-store snapshots once. Progress, failure-injection, cancellation,
   registration, and provider callbacks must not mutate the collections being
   enumerated. A rejected reentrant participant operation terminalizes capture
   even if its callback ignores the returned error.
2. Resolve requested roots and every redirect hop from owned metadata. Retain
   alias participants separately from final runtime output packages. Acquire
   package bytes and every owned companion into bounded owned storage; validate
   the package/segment pairing, parse metadata and exact references from those
   same bytes, and compare participating metadata with the captured Registry.
   A mismatch is `InputChanged`, not an instruction to silently update one part
   of the capture. Expand runtime and declared build dependencies to a fixed
   point before sealing. Missing undeclared inputs requested later fail closed.
3. Declare dependencies from captured inspection/settings, before package load
   and cache lookup. Resolve external file declarations into owned bytes and
   external value declarations into owned canonical values. Contributors receive
   only the capture context for declared inputs. Capture shader include closure
   and compilation options through ShaderBuild; the compiler must consume that
   closure without reopening authored paths.
4. Seal the input map. Cache-hit validation reads and hashes the exact output
   buffers it will retain, rather than hashing and subsequently reopening them.
   Fresh Cook uses the codec byte-context route and memory-owned bulk resources.
   Recursive hard/soft loads resolve through the sealed capture and cannot use
   the ordinary live loader as a fallback.
5. Reject participating packages already resident before this capture with
   `ResidentInputConflict`, whether marked clean or dirty. Do not evict them or
   discard edits. The initial supported caller is a quiescent Cook host with no
   participating resident packages; tests must release authored fixture packages
   before invoking it. Reusing clean resident packages requires a separate,
   tested generation-provenance mechanism and is not part of this plan.
6. Retain strong ownership of all capture-loaded objects and their backing
   resources. Contributor preparation may produce derived state on these
   objects but must not mutate authored fields. Existing serialization checks
   remain diagnostics for callback contract violations; they are not the
   isolation mechanism. Prevent owner-thread save/mutation/unload/shutdown and
   type/provider retirement from destroying active inputs. Cross-thread callers
   must fail before accessing unsynchronized Engine state.
7. Complete package and shader auxiliary captures, check cancellation, and
   compare current admission facts for all participants before detaching output.
   Compare package facts and fences, not the global catalog revision. Reentrant
   mutations/fences at discovery, load, contribution, and detachment boundaries
   must yield a typed failure. Unrelated publications remain permissible.
8. Detach save plans, dependency state, and auxiliary bytes together. Release
   capture-owned objects, resources, and provider tokens in dependency-safe order
   before opening output publication. Existing manifest-last publication owns
   rollback from this point onward. Changes to source inputs after detachment
   cannot alter the detached generation. Dry runs execute the same acquisition,
   hit checking, and capture rules, including auxiliary production.

No automatic retry is selected: return the first typed failure. A later explicit
run starts with a new capture. Cancellation is checked between bounded reads,
dependency expansions, loads, contributions, and detachment; it must never
publish partially collected inputs. Cancellation callbacks obey the same
reentrancy restrictions as progress callbacks.

### External process boundary

The input directory must be quiescent against out-of-process writes during
artifact acquisition, or supplied from an externally created immutable tree.
The engine cannot enforce cross-file atomicity against arbitrary native writes.
Opening two files or rehashing after contribution does not satisfy that
constraint. Package/companion digest validation detects mismatched captured
pairs; it does not establish a simultaneous filesystem snapshot. After sealing,
external edits are allowed because inspection, loading, bulk access, declared
external reads, and shader compilation all consume owned artifacts. Temporary
spill artifacts, if required by byte limits, must be private immutable copies
owned until the last reader finishes, never hardlinks to authored files.

### Query, admission, and dependency contracts

- Pure Registry queries accept package or exact object paths and immutable
  redirect-depth options. Their owned result contains requested/final paths,
  traversed aliases, final serialized class name, and existing structural
  failure diagnostics. No `DClass*` option belongs to the pure API. Descendant
  suffixes are retained through redirects; a package result cannot replace an
  exact object result.
- Separately named live Engine validation performs loaded-class compatibility
  and current participant admission. `FAssetRegistryAdmissionResult` carries
  `Admitted`, `ProjectionPending`, `ParticipantChanged`, or `NotFound`, plus the
  failed participant. It is a point-in-time result, never a retained permission
  to open current artifacts. Existing Load and mutation callers must retain all
  alias-hop checks when moved to the split API.
- `ECookInputStatus` distinguishes `InputChanged`, `ProjectionPending`,
  `ResidentInputConflict`, `ProviderUnavailable`, `UndeclaredInput`,
  `InvalidDependency`, `LimitExceeded`, `IoError`, and `Cancelled`. Carry the
  logical identity and operation stage into existing run/package result fields;
  preserve existing detailed asset errors instead of parsing message strings.
- `FCookBuildDependency` identifies a kind and stable logical name, with a
  canonical owned value. Kinds are source package, owned bulk, direct package,
  transitive package, external file, configuration value, and schema/producer
  version. Package hard dependencies automatically invalidate transitively;
  soft runtime references affect reachability but require explicit declarations
  to invalidate builds. Build-only declarations never add runtime outputs.
- Direct package dependencies include that package's source/bulk identity;
  transitive dependencies also traverse its declared build dependency graph.
  Expand to a sorted deduplicated set of reachable logical records using a
  visited set, not recursively nested hashes. Cycles terminate without
  self-referential hashes. Duplicate declarations are errors before graph
  expansion; repeated traversal of the same already validated record is valid.
- Persist dependency records with each package state. Encode kind tags, lengths,
  little-endian numeric values, names, and bytes explicitly before XXH3-128.
  Bound names, values, records per package, total bytes, and graph expansion;
  reject duplicate identities, unknown kinds, truncation, and trailing bytes.
  Introduce tool-only Cook-state version 2; version 1 becomes a full cache miss.
  DAST and runtime manifest versions remain unchanged by this state migration.
- Native contributors explicitly version behavior and declare all external
  inputs. Built-in families must be audited individually, including Material
  and EnvironmentLighting provider calls. A generic registration without a
  dependency declaration cannot opt into incremental reuse. A copied callback
  and its version number alone are not evidence of complete inputs.

### Protocol review gates

Before closing Stage 0, verify the proposed loader route can isolate all nested
loads and bulk reads without publishing or borrowing live objects; verify
provider retirement can defer owner destruction without waiting reentrantly;
and verify ShaderBuild can compile the fixed include closure. These require
source-level integration design, not a claim based on existing byte-view
parameters. Stage 0 remains open until those three seams are resolved.

Concrete follow-up entry points found during this review:

- `AssetPackageLinkerLoader.cpp` calls the public `LoadPackage` while resolving
  dependencies and uses global package load snapshots for rollback. A new
  top-level byte loader alone leaves both paths attached to live state. The
  capture loader must supply dependency resolution and capture-local rollback
  ownership throughout linker application, property reference decoding, and
  `PostLoad`.
- `FModularFeatureRegistry::InvokeSingle` pins a provider for a visitor and
  retirement reports `SelfWait`. A shader capture should stay inside one such
  visitor rather than retain a raw feature pointer across calls. External-store
  and Cook contributor registrations currently lack the equivalent owner
  protocol; their raw-pointer APIs cannot promise deferred destruction without
  an owner-facing registration change.
- `ShaderCompileService.cpp` resolves dependency paths and later calls
  `FSlangShaderCompiler::CompileSource`; `SlangSessionEnvironment.cpp` configures
  filesystem search paths. Supplying the top-level source string does not freeze
  includes. The same fixed source resolver must be supplied to dependency
  discovery and compilation, including generated Material sources.

## Implementation Stages

### Stage 0: Define the input capture and admission protocol

- [x] Confirm the hidden fence/type dependencies and identify current Cook
  consumers and existing regression-test expectations.
- [x] Inventory participating mutation, save, scan, load, external-store, class
  registration, and contributor lifetimes. Establish the actual thread contract
  and lock ordering before adding locks or holding guards across callbacks.
- [ ] Select and document one implementable Cook input protocol: fixed input
  artifacts and protected object capture, or a scoped read session coordinated
  with all participating writers. Define treatment of preloaded dirty/stale
  objects, package companions, external stores, and type/provider retirement.
- [x] Define what happens when an external process edits content outside the
  engine protocol. Prefer detached artifacts that bind inspected, fingerprinted,
  loaded, and contributed data to the same generation. State unavoidable
  environment constraints explicitly; do not promise arbitrary concurrent-file
  consistency using only a final rehash.
- [x] Define concrete public query/admission result types, build dependency
  kinds, capture context ownership, cancellation, and failure propagation.
- [ ] Record whether a class/schema input is captured as stable data or pinned
  under a lifetime contract. Fix external root-provider snapshots for the run.

Completion condition: a single reviewed execution protocol covers the interval
from dependency discovery through output capture, including reentrant callbacks,
with no claim that revision or hash equality substitutes for input protection.
Stages 1-3 depend on this decision; do not ship a partial safety replacement.

### Stage 1: Separate metadata queries and live admission

- [ ] Implement package and exact object-path resolution using only the owned
  catalog and explicit immutable arguments. Remove implicit loaded-class checks
  and fence reads from snapshot code.
- [ ] Keep live resolution checks explicit and preserve alias-hop fencing,
  missing/cyclic/corrupt redirect diagnostics, class compatibility, and object
  descendant behavior. Avoid silently weakening current Load/mutation callers.
- [ ] Implement participant-scoped validation or admission under the Stage 0
  protocol. Compare the participating facts rather than unrelated revisions.
- [ ] Migrate snapshot consumers, including Cook reachability, to the split
  interfaces. Clearly label operations that inspect files or external stores;
  they are not pure snapshot queries.
- [ ] Replace the existing fence-sensitive snapshot expectation and test
  snapshots captured before/during/after fences, later publications, unloaded
  class names, redirect chains, and exact object-path resolution.

Completion condition: old views remain stable; current operations still reject
unsafe participants; bounded Registry and affected caller tests pass.

### Stage 2: Introduce explicit package build dependencies

- [ ] Implement dependency declarations and reevaluation before cache lookup,
  without requiring package loading merely to discover cache validity.
- [ ] Supply automatic source/owned-bulk identities and deliberate package build
  dependency rules. Allow explicit external file, configuration/value, and
  schema/version dependencies with stable logical identities.
- [ ] Separate direct from transitive dependency semantics, define cycle
  termination, and keep build-only inputs out of runtime reachability.
- [ ] Hash canonical names, kinds, values, and relevant settings with explicit
  framing; exclude process revisions, timestamps, absolute workspace/output
  paths, scheduling, and DDC locations from persistent identity.
- [ ] Persist canonical per-package dependency records in Cook state, validate
  bounds/corruption/duplicates, and invalidate incompatible prior state safely.
- [ ] Migrate contributor registration and family integrations. Audit hidden
  inputs and define explicit version bumps for native behavior changes; do not
  present arbitrary callback output as automatically complete dependency data.
- [ ] Test input changes with unchanged metadata, owned-bulk changes, direct and
  transitive dependencies, build-only inputs, cycles, ordering, duplicates,
  external values, and Cook-state round trips and corrupt/truncated data.

Completion condition: package reuse is explained by persisted build inputs;
equal inputs reuse across unrelated Registry publications and changed declared
inputs invalidate even if catalog revision or file timestamps remain unchanged.

### Stage 3: Bind Cook discovery and output to captured inputs

- [ ] Integrate Stage 0 input capture with Registry metadata, source/bulk bytes,
  exact reference inspection, loaded object state, and contributor execution.
- [ ] Include requested aliases and intermediate redirects in protected inputs
  even though they are omitted from the final runtime package list.
- [ ] Bind fingerprint evaluation and contributed bytes to the same captured
  input generation. Handle dirty/preloaded objects and provider retirement
  explicitly rather than assuming disk hashes describe resident objects.
- [ ] Validate Cook hits against dependency values and output integrity; apply
  the same input contract to hits, fresh captures, and dry runs.
- [ ] Preserve cancellation, failure injection, shader auxiliary output, and
  existing manifest-last store behavior. Release input resources once detached
  output no longer needs them; avoid holding unrelated locks through output I/O.
- [ ] Add coordinator-level tests that change/fence inputs at deterministic
  discovery, load, contribution, and prepublication boundaries. Verify either
  consistent captured output or typed failure with the prior manifest intact.
- [ ] Test unrelated Registry changes, alias changes, external-root changes,
  schema/contributor lifetime, resident/disk disagreement, and incremental hits.

Completion condition: end-to-end coordinator tests exercise actual captured
outputs and cache reuse. Pure-hash unit tests alone cannot satisfy this stage.

### Stage 4: Validate and document the final contracts

- [ ] Update the authoritative catalog/mutation and asset-data lifecycle
  contracts, including revision scope, pure query limits, admission lifetime,
  dependency declaration responsibilities, and Cook-state migration.
- [ ] Run affected native validation under the repository test workflow and
  relevant integration targets. Determine CPU versus GPU requirements from
  actual changed behavior; record any unavailable required acceptance lane.
- [ ] Verify public header/export coverage, existing runtime loading and
  mutation recovery, cook-hit repair, cancellation, and manifest rollback.
- [ ] Review performance of dependency traversal and input capture: reuse
  per-run immutable inputs, avoid hashing each shared dependency for every
  package, and bound retained bytes and dependency-state size.
- [ ] Record exact validation evidence, commit each validated stage with this
  plan's provenance, and mark completion only after all required gates pass.

Completion condition: documented behavior matches the tested implementation;
no unvalidated draft or unresolved input-isolation claim remains.

## Validation and Handoff

Follow [Build and Run](../Agents/BuildAndRun.md) and
[Testing](../Agents/Testing.md). Query registered targets rather than inferring
targets from legacy source directory names. Initial relevant registered targets
include `PackageRegistryContractTests`, `AssetCookTests`, and
`AssetPackageTests`; use affected selection for handoff and exact integration
coverage when affected behavior requires it.

Stage 0 inventory validation: `doc validate --scope changed` passed for the
single changed plan. The command used command-local Git safe-directory
configuration for this worktree; no global Git settings were changed. No native
test was run for this documentation-only inventory. This receipt does not close
Stage 0's protocol review or any implementation acceptance gate.

Stage 0 external-root foundation validation (2026-09-07):

- The exact reentrant-registration regression passed.
- `test affected` rebuilt successfully and ran 79 targets: 78 passed, including
  both new package tests (registration changes reject partial capture; owned
  roots survive provider retirement), existing Cook tests, external-reference
  tests, Registry tests, and package loading/mutation coverage.
- `VulkanRHIIntegrationTests` crashed during the affected run. The retained
  diagnostic is `Build/.agent-state/logs/20260907-155748-589394-32936-ctest.log`;
  injected Vulkan creation failures precede the crash and are not by themselves
  proof of its cause. The exact target rerun passed all 68 cases in 11.31 seconds
  without a source change; its log is
  `Build/.agent-state/logs/20260907-160031-546888-23352-VulkanRHIIntegrationTests.log`.
  This does not retroactively make the affected aggregate a pass.
- An earlier affected build timed out during GoogleTest discovery; the later
  build completed successfully without a source fix. No recovery or rebuild was
  required. The shared dependency-lock permission issue was resolved through
  authorized execution, without changing filesystem ACLs.

The source tree was clean before investigation. No native build or test was
run for the preliminary draft. `test affected --explain` was used only to
inspect selection; it is not validation evidence. Resume with Stage 0 and
inspect current code before reusing any draft fragments.

## Related Code

- `Engine/Source/Runtime/AssetRegistry/Private/AssetRegistryState.cpp`
- `Engine/Source/Runtime/AssetRegistry/Private/AssetRegistryStateInternal.h`
- `Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/Catalog.h`
- `Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/References.h`
- `Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/Publication.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPublicationCoordinator.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookCoordinator.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/EngineCookContributors.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetRuntimeStateInternal.h`
- `Engine/Source/Runtime/Engine/Public/Asset/Cook.h`
- `Engine/Source/Runtime/Engine/Public/Asset/References.h`
- `Engine/Tests/Native/AssetRegistryTests/Private/PackageRegistryTests.cpp`
- `Engine/Tests/Native/AssetTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/AssetTests/Private/PackageTests.cpp`

## References

- [Asset catalog and mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [UE temporary Registry state API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/AssetRegistry/AssetRegistry/IAssetRegistry/InitializeTempor-?application_version=5.5)
- [UE Cook development reference](https://dev.epicgames.com/documentation/unreal-engine/cplusplus-cooking-development-reference)

UE references motivate responsibility boundaries, not claims about Durin's
current behavior or proof that UE supplies an identical fence/read-lease model.
