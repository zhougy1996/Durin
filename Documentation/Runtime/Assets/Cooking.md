# Asset Cooking and Publication

Summary: Define offline Cook inputs, dependency reuse, typed failures, and transactional output publication.

Last reviewed: 2026-09-21

## Coordinator and input ownership

`FCookContext::AddPackage` accepts canonical cooked package bytes; package
serialization captures any `FBulkData` ranges and produces the optional raw
segment. `AddRawPackage` admits an already laid-out opaque segment. Asset families
contribute through class-keyed registrations owned by `RegisterEngineCookContributors`; individual family
Cook methods are private.

A coordinator run captures the contributor registration map before its first
cancellation or provider callback. Resolution for every package uses that map.
Unregistering retires a contributor from future runs without waiting for active
runs; replacing it cannot change a captured run's selection. Registrations may
supply `LifetimeOwner` to retain callback state and native code. An empty owner
requires the caller to guarantee their process lifetime. Callback wrappers are
destroyed before the owner, outside the registration mutex, including reentrant
retirement. The run releases its registrations before opening the output store,
or on failure, cancellation, or dry-run return. The standalone host loads types
and providers before package work and keeps their code alive through shutdown.
Arbitrary native callback state still requires explicit dependency declarations.

CMNF entries are sorted by normalized cook-relative path and record kind, required
flag, byte extent, and XXH3-128 digest. Raw companions use `PackageBulk`;
package-only output has no companion entry.

Win64/Game Cook also supplies `Shaders/ShaderLibrary.dslb` as a
`ShaderLibrary` auxiliary output. It is detached before the store transaction,
validated by its producer, staged and committed with package outputs, and
recorded in CMNF. Failure at its stage or commit participates in the same
reverse-order rollback; it is never published beside the Cook transaction.

ShaderBuild accepts `FShaderCompileOptions::SourceArtifacts` for a fixed input
session. `FShaderSourceArtifacts` copies caller buffers and exposes only const
access. Dependency discovery and compilation use its lexical file identities;
missing imports/includes cannot fall back to disk. These service requests bypass
live metadata and shader caches. Sessions reject noncanonical paths, names over
4096 bytes, more than 65536 files, files over 64 MiB, or total source bytes over
512 MiB. Ordinary shader requests retain the mounted-source/cache route. Cook
hashes mounted source identities and search roots once during dependency discovery,
then compiles Material programs and the detached runtime library from the stable
files through ordinary provider invocations. It does not retain a whole-run source
copy or pin an invocation across the run. The standalone host keeps provider code
loaded through work completion and object/resource shutdown. Explicit fixed-input
service requests retain their no-fallback behavior.

Cook reachability resolves explicit, built-in, and registered external runtime
roots before traversal. Redirectors are authoring-only: references are rewritten
to final real identities and redirector packages are omitted. Missing targets,
cycles, type mismatches, corrupt aliases, duplicate output identities, or
incomplete reference projections fail before manifest publication.
External-store registration may supply a capture owner that retains the store
and provider code until `CaptureAssetReferenceStores` finishes all callbacks.
Reentrant retirement still rejects the capture, but cannot destroy a pinned
provider on its callback stack. Unregistration detaches the owner before running
its destructor. Other mutation/fix-up callbacks retain their caller lifetime
contract; a capture owner does not extend those operations automatically.
Production Cook runs once in `DurinAssetTool cook`, then exits. The invoking
workflow saves edits and finishes imports first, and keeps all participating
packages, companions, external files, shaders, configuration and native code
stable until exit. Unsaved editor memory is excluded by the process boundary.
Concurrent edits are unsupported: discard the output and rerun. Cook neither
prevents nor guarantees diagnostics for such edits.

The host rejects overlapping output destinations after resolving existing
filesystem aliases. DDC, shader intermediates and logs use separate writable
locations; see the [tool workflow](../../Development/Tooling/DurinDevTool.md).
Cook never saves or repairs authored inputs. Shutdown joins compilation and tasks,
releases assets/resources, then releases the Renderer inventory library.

The coordinator discovers external root stores once, and inspects exact references
from stable package files. The standalone `BuildCookReachability` helper remains
an independent query for callers that need reachability without performing Cook.
It reports the full authored reference closure and does not supply persistent
incremental identity. The coordinator separately selects runtime packages using
the Cook retention policy: references through reflected top-level `EditorOnly`
fields remain validated build inputs but do not add runtime packages when those
fields are stripped. Unknown custom archive fields retain runtime dependencies.
Material function calls use this policy: nested function package bytes and schemas
invalidate the material Cook cache, while cooked materials load their accepted
program without function assets. Material and instance Cook recipe version 5
includes this dependency policy and invalidates pre-removal payloads. DMAT version
7 omits the obsolete authored Program version word; typed IR, generator, compiler
envelope and payload versions remain independently validated.

## Dependency discovery and reuse

`FCookCoordinator` requires the object owner thread and rejects nested runs before
callbacks. It is an internal orchestration component, with no live-editor or
concurrent-session guarantee. `FCookDependencyDiscovery` retains dependency metadata,
content hashes and small declared values. It does not own an isolated object graph
or retain whole-run package/bulk copies. Ordinary loading provides recursion,
aliases, cycles, residency and file-backed lazy bulk. Existing residents in this
process are valid; ordinary non-Cook load/mutation admission remains unchanged.

Contributors prepare derived state and return detached save plans without saving
authored files. `FScopedOfflinePreparation` selects synchronous Material work and
lets StaticMesh build its detached serialization product without editor compilation.
It does not gate mutation or loading. The host must finish owned work before
unloading provider code. `DeclareDependencies` executes before every cache lookup without loading
objects. Absence of a callback disables reuse, including transitive contributors
without declarations. Declarations use stable logical names and these semantics:

- Source package and owned bulk identities are automatic. Hard package references
  are transitive build inputs; soft references affect runtime reachability without
  implicitly becoming build inputs.
- A direct package input observes its source and bulk, including redirect aliases
  and final target. A transitive input also visits its declared build inputs.
  Cycles terminate once per node. Build-only packages do not become runtime roots.
- External files are hashed during discovery and read again when requested; their
  locator is excluded from identity. Configuration and schema/producer values are owned canonical bytes.
  Contributors read them through `FCookContext::ReadDeclaredInput`; undeclared
  access fails the run even if its immediate error is ignored.
- Names, kinds, lengths, and values are canonically framed. Contributor/family
  versions, relevant reflected schema, target/profile, and editor retention are
  included. Process revisions, timestamps, absolute paths, scheduling, and DDC
  location are excluded. Native constructor defaults, custom serializers, build
  algorithms, and other unreflected behavior require an explicit contributor or
  producer version bump; callback output is not automatically a complete recipe.

Built-in Texture2D, TextureCube, VolumeTexture, and StaticMesh declarations include
native recipe-provider descriptors. Material declares shader source/compiler
identity evaluated once per run without retaining source bytes; compiler calls
use ordinary stable files. Sky Light references are normal TextureCube package
dependencies; filtered lighting is transient GPU state, with no external IBL
payload. StaticMesh pending authored mutation remains an invalid offline input.

`CookState.bin` version 2 persists canonical per-package dependency records,
separately from CMNF. Version 1, invalid, duplicate, truncated, or incompatible
state becomes a full cache miss. A hit additionally validates output size and
digest and retains those exact verified buffers. Missing/corrupt output is
recaptured and repaired. Hits, fresh captures, and dry runs share dependency discovery and
auxiliary-library production. DDC hits, rebuilds, captures, and Cook hits remain
distinct provenance.

Discovery limits are 65536 packages and graph/runtime edges, 256 MiB per read file, and
1 GiB of accounted retained input data. Dependency sets use at most 65536 records,
1 MiB per value, and 64 MiB encoded data; the prepared graph shares source hashes
across expansions. Cook state is bounded to 256 MiB and detached output to 1 GiB.
These bound payload storage rather than total process RSS or native callback
allocations. The JSON `peakCapturedBytes` field accounts retained dependency values
and detached outputs; it excludes temporary codec/compiler buffers and ordinary
loader/resource allocations. Shader identity evaluation separately caps total source bytes at 512 MiB and checks
cancellation between directory entries, read chunks, and library requests.

## Results and failure ownership

Cook contributor registration returns `FCookContributorRegistrationResult`,
with success derived from its error code and the new handle exposed separately.
Failures distinguish class/name/callback/version requirements and duplicate class
routes, retaining contributor name, qualified class name and version context.
Engine family batch registration returns the first typed failure and removes only
handles added by that batch. Existing caller-owned handles remain unchanged.

Cook discovery retains its first `FCookInputFailure` in its own failure channel;
`FCookRunResult::InputDiagnostic` receives that owned record alongside the run
`FCookInputResult` status and diagnostic text. Contributor callbacks return
`FCookContributionResult` directly; dependency graphs adapt to Cook input
status without passing through asset read or write classifications. Cancellation, input write conflicts, file IO,
file/aggregate byte and package-count limits, unknown packages, undeclared
values and missing readers have typed codes with owned identities and limits.
File failures retain the full IO operation, native error, path, offset and size.
Declaration validation uses distinct count, name, duplicate, package, external
file, value, retained-storage and reserved-kind errors. These own the declaring
package, kind/name, file and applicable byte/count bounds. Invalid package paths
retain their complete `FObjectPathError`; payload-shape rejection remains separate
from path parsing. Declaration failure prevents contributor execution and output
publication, while an earlier retained cause remains valid across later runs.
Dependency discovery has no string-based failure entrypoint. Root counts and
classes, runtime edge limits, empty runtime selections, Bulk size/digest mismatch,
schema availability, schema depth/field/type/encoding limits and retained schema
or dependency storage all produce Cook-owned input failures. Schema failures retain
class/member identities and limits; Bulk failures retain expected and actual
sizes and digests. Reclassifying a missing reference as MissingDependency keeps
the underlying diagnostic text. Projection disposition and the first input
failure remain independent of diagnostic formatting.

The asset-result adapter formats explicitly and preserves the existing
classification; discovery retains its first failure and independent input status.
File reads publish bytes only after every chunk succeeds; failed reads leave the
output empty. A context with no reader also clears output before rejecting it.

Codec and graph results own typed errors, record/package identities, indices,
limits, and nested causes. Their output rules are:

| Result | Failure and publication behavior |
| --- | --- |
| `FCookDependencyCodecResult` | Encoding/decoding clear outputs; fingerprinting returns the same result and clears its output. Canonical ordering and byte framing are preserved. |
| `FCookDependencyGraphResult` | Initialization clears prepared nodes; expansion clears output records. Retains conflicting dependencies, aggregate counts, and codec causes. |
| `FCookStateResult` | Clears byte/state outputs; publishes only a complete candidate. Dependency framing errors remain distinct from payload errors. |
| `FCookManifestResult` | Encoding clears bytes; decoding resets the manifest. Encoding validates through decoding; errors retain size/checksum and target context. |

Discovery's `ToAssetResult` adapter formats graph/codec context while the result
is alive, preserving CorruptFile classification without a write outcome.
Cook-owned codec/state/publication statuses are not embedded in ordinary asset
diagnostics. The output store retains complete state and manifest causes.

`ResolveCookedPackagePath` and `ResolveCookedCompanionPath` return
`FCookedPathResult`, distinguishing root, virtual path, mount, normalization,
extension and containment failures. Results own their root and path context;
failed resolution clears the output path. Output-store validation retains the typed path cause.

`ValidateCookOutputRoot` returns `FCookOutputRootResult` and performs no writes.
Its error distinguishes absolute-path requirements, filesystem inspection and
canonicalization, authored-tree overlap, entry bounds, and alias escape. Results
own output/related/resolved paths, system error codes, and applicable counts.
The store retains the typed root cause; the coordinator and command host format
errors at their presentation boundaries.

`FCookContext` owns pending package buffers and target settings, without a disk
output root. Its `AddPackage` overloads return `FCookPlanResult`, distinguishing
invalid path/source identity, empty bytes, duplicate path, invalid package and
projection failure. Errors retain the virtual path and complete serialization
result for projection failures; admission failure publishes no plan.
Family contribution returns `FCookContributionResult`, retaining object/virtual
paths, target settings, material revision and nested plan causes. Target, missing
derived state, stale material revision/contract/dependencies and unsupported
classes have distinct errors. The registration callback uses `ToAssetResult` to
format the contribution diagnostic at the callback boundary; the asset result
does not embed a second contribution result.
Engine registration callbacks also retain typed authoring-only, class mismatch,
pending source mutation and missing recipe/shader-input failures. Class failures
own expected/actual identities; dependency failures own provider and package
identities. Their asset adapters retain the existing UnsupportedProperty,
TypeMismatch or InUse classification. The coordinator retains the complete
`ContributionCause`, package and contributor identity before formatting the
run presentation. Starting another run clears this prior failure state.
Coordinator capture failures retain `FCookCaptureResult` in `CaptureCause`.
Finalization errors own the complete `FCookPlanError`, including nested asset or
path-resolution causes; successful finalization with an unexpected plan count
retains actual and expected counts instead. Both carry package and contributor
identity. Failed capture never reaches output publication; later runs reset the
cause without invalidating copies retained by callers. Formatting remains at the
run presentation boundary.
`FCookRunResult::Error` uses `ECookRunError`; its boolean success depends only
on `None`. Run disposition remains in `Status`, while `bDryRun` records request
mode separately. The coordinator writes typed codes for every terminal path.
`CookRunCodeName` preserves command/JSON code names, including successful
`dry-run` versus `succeeded`; no string code remains in the run result. Per-package
results also store no code or diagnostic text: command/JSON uses
`CookPackageStatusName` and `FormatCookPackageResult` with the structured status,
while package identity, contributor, stage and byte counts remain independent.
`FCookRunResult` stores no
diagnostic text; `FormatCookRunError` formats codes, owned identities, counts and
nested causes at command and test presentation boundaries. Fingerprint failures
retain their codec cause; output-budget failures retain requested package/bulk
bytes, retained bytes and the maximum. Failure injection captures stage/index
and at most 2048 bytes of external provider text.

Run preflight retains complete output-root, project-settings and default-level
path failures in `OutputRootCause`, `SettingsCause` and `DefaultLevelCause`.
Shader library failures retain `ShaderCause` for both failure and cancellation
outcomes. These owned causes survive formatting at the run presentation boundary and
are reset with the rest of the result at the start of each run. Project settings
still expose their existing typed-code/text result and Cook does not reconstruct or classify that text.

`AddRawPackage` uses the same typed result, retaining package/segment byte counts
for empty payloads and the segment size/limit for oversized segments; raw
admission failures also leave pending plans unchanged. `TakeSavePlans` consumes
those buffers and returns canonical detached plans with `FCookPlanResult`. Invalid targets retain platform/profile, package
canonicalization retains the complete asset cause, and path resolution retains
the owned registry resolution result. Duplicates after canonicalization have a
distinct code. Failure consumes pending work and leaves the output empty. Direct
family callers use `PublishCookContext(Context, OutputRoot)`; production uses the
coordinator. Direct publication returns `FCookContextPublishResult`, distinguishing
finalization from publication failure while retaining the output root and complete
nested plan or store result. It has no string output parameter; the formatter
renders nested causes only at presentation. Both publish
through the same output store. The C++ retained-byte
metric is `PeakRetainedBytes`; JSON schema v1 keeps `peakCapturedBytes` and the
`rangeReadCount` field (always zero) for compatibility.

`FCookPublishResult` retains typed root/path/package/manifest/state causes when
those boundaries reject publication. `ValidationCause` retains request/plan,
opaque-segment, package-identity, raw-bulk-closure and auxiliary-output categories
with owned root/path, index and request target context. Rejection precedes output
creation. `OperationCause` separately retains
filesystem/cancellation operation classification, stage, owned path and available
system error code. `FCookPublishResult` has no engine-owned diagnostic text;
success derives from `ECookPublishError::None`. Caller-injected text is held only
in `InjectionCause`, bounded to 2048 bytes with stage/index context. The coordinator
preserves the complete failed result in `FCookRunResult::PublicationCause` before
presentation formatting. Status and transaction timing remain independent
from the cause; existing cleanup and manifest-last publication order are unchanged.

## Output transaction

All save plans are detached before `ICookOutputStore` opens its transaction.
The local loose store enforces one writer per output root, stages and validates
every changed file, retains overwritten bytes, commits segments before packages,
then `CookState.bin`, and commits `CookManifest.bin` last. Failure or
cancellation before the manifest commit restores the prior closure in reverse
order. Unchanged validated plans preserve bytes and timestamps.
Only after manifest publication may paths owned solely by the previous
manifest be removed; unowned files are never cleanup candidates.

## Related documentation

- [Asset data lifecycle](AssetDataLifecycle.md)
- [Cook command workflow](../../Development/Tooling/DurinDevTool.md#project-cook)
