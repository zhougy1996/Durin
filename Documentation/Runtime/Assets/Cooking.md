# Asset Cooking and Publication

Summary: Define offline Cook inputs, dependency reuse, typed failures, and transactional output publication.

Last reviewed: 2026-10-18

## Coordinator and input ownership

Target interpretation and immutable operation capabilities follow
[target platform build policy](TargetPlatforms.md). Each run owns a copy of its
request before callbacks, including canonical material variant selections.
Family source roots own native recipe declarations; material configurations are
declared before Cook lookup. Unrelated family settings do not enter every DDC key.

Engine package capture prepares material programs and StaticMesh render/collision
products once before Archive discovery/emission. A bounded invocation-scoped
table retains detached bulk by object handle, package revision and exact target.
Serializers only consume these values; direct Engine package serialization uses
the same preparation entrypoint. Cancellation/failure destroys the table before
publication. Preparation does not depend on an accepted live material render
program or apply a compiled candidate to the asset.

`FCookContext::AddPackage` accepts canonical cooked package bytes; package
serialization captures any `FBulkData` ranges and produces the optional raw
segment. `AddRawPackage` admits an already laid-out opaque segment. Asset classes
own their Cook lifecycle overrides. `SaveCookPackage` and direct package saves use
one common package capture; object hooks cannot publish plans. There is no Cook
contributor registry, registration scope, handle, or registration snapshot.

The standalone host loads types and providers before package work, joins their
operations, releases objects/resources, then unloads native code. Class hooks have
the same lifetime as their reflected type; operation callbacks do not extend a
module's lifetime independently.

CMNF entries are sorted by normalized cook-relative path and record kind, required
flag, byte extent, and XXH3-128 digest. Raw companions use `PackageBulk`;
package-only output has no companion entry.

Win64/Game and MacOS/Game Cook with retained runtime packages also supply `Shaders/ShaderLibrary.dslb` as a
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

## Object lifecycle and selection

`DObject` provides default begin/completion/clear, `PreSave`, dependency-event and
target-eligibility hooks. `FObjectCookContext` carries generic target/request
values, declarations and typed results without depending on Engine or its build
providers. Required named request values become captured dependencies. Native
`AppendToClassSchema` hooks bind through `FClassParams` and intrinsic construction;
`DClass::AppendCookSchema` visits distinct hooks root-to-leaf without CDO access.
Static schema hooks do not call their parent; object virtual overrides do.
Family hooks write declarations directly into this context. Material shader identity
and the canonical configuration set are each captured once through required request
values; material Cook schema version 7 invalidates packages predating the binary
MacOS Shader selection. For MacOS, the required Shader build identity also binds
the native Metal compiler and SDK provenance before package reuse. Texture and
Mesh schema evaluation does not parse material request values.
Class schema collection clears the package identity and accepts only shared
configuration/producer-version values. Declare package and external-file inputs
in `OnCookEvent`, where the instance and package are available. This prevents a
once-per-class cache from reusing declarations chosen for the first package.

Engine starts independent selected objects before joining, subject to bounded
admission. Mesh Begin captures immutable source/settings and physics-input tasks;
worker work builds/encodes detached products without reading Mesh or BodySetup.
Material Begin snapshots every configuration on the game thread and submits
`FMaterialBuildOperation` requests; completed variants are encoded in canonical
order, independent of callback order. Neither path publishes live editor resources.
Texture retains its existing platform-cache lifecycle. Small/cooked-only paths and
explicit synchronous adapters remain supported. Without a running task scheduler,
detached adapters execute synchronously; ordinary load-time compilation retains its
existing policy.

`FAssetPackageSerializationOptions` defaults to four outstanding preparation
reservations, 512 MiB of working reservations, and a separate 1 GiB retained-product
limit. Reserve before capturing inputs; admission can join/adopt earlier operations
to make progress under smaller budgets. Mesh uses checked geometry/physics envelopes;
Material reserves 64 MiB per variant for snapshot, compilation and encoding overlap.
Oversized work fails explicitly. Optional owner-thread `PreparationMetrics` report
reservation peaks and retained product bytes, not physical allocator measurements.
Scheduling limits do not enter recipe or output identities.

Completion is distinct from success and from request termination. Common teardown
cancels all outstanding work, joins owned tasks/requests and clears every started
object, including failed, canceled and nested captures. Worker callbacks never need
a game-thread pump to finish. Request cancellation is polled on the owner thread
and latched into provider tokens. Joining is cooperative and has no hard timeout;
a lifecycle query still reporting incomplete after its join adapter fails the save.
Cross-package overlap and simultaneous targets on one object are unsupported.

`PreSave` executes once per selected object before archive discovery/emission,
for authored and cooked saves. The persistent graph remains frozen: no new/deleted
exports, reparenting or generated packages. Hooks may adjust supported transient
save state but cannot rewrite authored state, accepted live programs or captured
build inputs. Package edit revision and Mesh source/metadata/render and Material
parent/function revisions are checked before product consumption and after capture.
These guards supplement graph equality; direct untracked native writes remain
unsupported. Heavy construction/DDC work stays outside serialization and PreSave.
Detached products are bounded to one capture and exact target/revision.

`IsEditorOnly` and `NeedsLoadForTargetPlatform` determine Cook exclusion, including
excluded ancestors. Material functions are editor-only but remain build inputs.
A kept hard reference to a valid policy-stripped object serializes null, within
or across packages, without modifying the live pointer or retaining a dangling
import. Invalid/missing/wrong-type references and explicit save omissions retain
their own validation. Soft paths preserve authored identity but do not promote a
stripped target into runtime output. Retaining diagnostic editor fields cannot
resurrect an excluded object. A package with no runtime exports is `omitted`, even
when explicitly requested; mixed packages retain their eligible exports.

Authored saves invoke the dependency event with `bCooking == false`, persisting
package build edges separately from hard/soft runtime references in Registry v3.
Other declaration kinds are Cook-only. Metadata aids scheduling; current Cook
object declarations own direct/transitive semantics, and metadata-only build
inputs observe source/bulk directly. Registry v2 remains readable. See
[package persistence](../Core/PackagePersistence.md) and
[package format](AssetPackages.md#defaults-and-version-policy).

## Dependency discovery and reuse

`FCookCoordinator` requires the object owner thread and rejects nested runs before
callbacks. It is an internal orchestration component, with no live-editor or
concurrent-session guarantee. `FCookDependencyDiscovery` retains dependency metadata,
content hashes and small declared values. It does not own an isolated object graph
or retain whole-run package/bulk copies. Ordinary loading provides recursion,
aliases, cycles, residency and file-backed lazy bulk. Existing residents in this
process are valid; ordinary non-Cook load/mutation admission remains unchanged.

Object overrides prepare derived state without saving authored files. The common
publisher captures each retained package once. `FScopedOfflinePreparation` selects
ordinary synchronous load-time preparation and suppresses StaticMesh editor
compilation during offline loading. Explicit detached Cook preparation can run
asynchronously within that scope. Ordinary mutation/loading admission is unchanged.

Class schemas are evaluated before reuse without constructing assets. Object
`OnCookEvent(PlatformCookDependencies)` declarations and eligibility are evaluated
on a projection miss and recorded for reevaluation. Projection
`bCanReuseWithoutLoading` is independent of current dependency completeness and
output fingerprint comparison. Arbitrary instance-computed configuration/schema
values require loading and reevaluation each run; unchanged complete values can
still hit outputs without preparing or recapturing. Hooks with genuinely untracked
inputs set `bDependenciesComplete = false`; incompleteness propagates through
transitive build edges, while direct edges observe only source/bulk. Empty default
hooks follow the complete-declaration contract and need no family registration.

Declarations use stable logical names and these semantics:

- Source package and owned bulk identities are automatic. Hard package references
  are transitive build inputs; soft references affect runtime reachability without
  implicitly becoming build inputs.
- A direct package input observes its source and bulk, including redirect aliases
  and final target. A transitive input also visits its declared build inputs.
  Cycles terminate once per node. Build-only packages do not become runtime roots.
- External files are hashed during discovery and read again when requested; their
  locator is excluded from identity. Configuration and schema/producer values are owned canonical bytes.
  Preparation hooks read them through `FCookPreparationContext::ReadDeclaredInput`; undeclared
  access fails the run even if its immediate error is ignored.
- Names, kinds, lengths, and values are canonically framed. Family behavior
  versions, relevant reflected schema, target/profile, and editor retention are
  included. Process revisions, timestamps, absolute paths, scheduling, and DDC
  location are excluded. Native constructor defaults, custom serializers, build
  algorithms, and other unreflected behavior require an explicit class-schema or
  producer version bump; callback output is not automatically a complete recipe.

Built-in Texture2D, TextureCube, VolumeTexture, and StaticMesh declarations include
native builder versions. Material declares shader source/compiler
identity evaluated once per run without retaining source bytes; compiler calls
use ordinary stable files. Sky Light references are normal TextureCube package
dependencies; filtered lighting is transient GPU state, with no external IBL
payload. StaticMesh pending authored mutation remains an invalid offline input.

Class-owned `VersionCoveredClasses` explicitly qualifies exact class names after
an audit of inherited/nested native behavior. New subclasses do not inherit that
qualification implicitly. `UsedCustomVersions` identifies current local definitions,
including policy and Cook-only paths before projection reuse. Source-recorded GUIDs
also contribute their current local versions. Missing definitions fail discovery.
`ConstructedClasses` adds a bounded closure of reflected/native schemas even when
those classes are absent from authored exports. This metadata uses the object/type
system and introduces no separate evaluator registry.

Qualified packages hash only used custom versions and class-declared governing
configuration inputs. Unknown/incomplete class coverage retains all registered
versions and conservative request settings. Built-in texture leaves, StaticMesh and
material owners declare audited coverage; unaudited children such as expression,
import and BodySetup classes retain package fallback. Material owners conservatively
retain the material-domain versions. Build-only classes remain dependencies after
runtime stripping. Automatic hard imports remain transitive; explicit declarations
retain their direct/transitive semantics. Native behavior changes still require
an appropriate schema/version bump.

`CookState.bin` version 5 persists output dependencies and a separate input projection
table, including omitted/build-only packages. Projections bind source, bulk and current
reflected/native/version/schema inputs to object selections and descriptors. External
file descriptors keep locators separate from content identities. Class schemas are
recomputed each run. Earlier, invalid, duplicate, truncated or incompatible state
becomes a safe miss without legacy migration readers. Cook never repairs or resaves
authored inputs; authored package compatibility remains independent of CookState.

A hit additionally streams output size/digest verification and retains only output
metadata matched against the previous manifest. Missing/corrupt output is recaptured
and repaired. Missing/invalid prior manifests also force recapture. The store rechecks reuse
references under its writer lock before committing; a reference changed after discovery
fails publication rather than staging empty bytes. Byte-backed plans can still repair
corrupt output. Hits, fresh captures and dry
runs share dependency evaluation; retained runtime packages also produce the
auxiliary shader library. Package reports distinguish Cook hits, captures and omissions;
DDC hits and rebuilds belong to build operations. Retired v2 package statuses are accepted
only for reading older reports. All-omitted runs publish valid empty package manifests/state without
an unnecessary shader library.

Discovery limits are 65536 packages and graph/runtime edges, 256 MiB per read file, and
1 GiB of accounted retained input data. Dependency sets use at most 65536 records,
1 MiB per value, and 64 MiB encoded data; the prepared graph shares source hashes
across expansions. Cook state is bounded to 256 MiB and retained detached output bytes
to 1 GiB; metadata-only reuse references do not consume that output-byte budget.
These bound payload storage rather than total process RSS or native callback
allocations. The JSON `peakCapturedBytes` field accounts retained dependency values
and detached outputs; it excludes temporary codec/compiler buffers and ordinary
loader/resource allocations. Shader identity evaluation separately caps total source bytes at 512 MiB and checks
cancellation between directory entries, read chunks, and library requests.

## Results and failure ownership

`FObjectCookResult` separates generic typed failure/cancellation from completion.
Engine preparation retains its family-specific `FCookContributionResult` cause;
registration errors and duplicate route/name diagnostics no longer exist.

Cook discovery retains its first `FCookInputFailure` in its own failure channel;
`FCookRunResult::InputDiagnostic` receives that owned record alongside the run
`FCookInputResult` status and diagnostic text. Engine preparation returns
`FCookContributionResult` directly; dependency graphs adapt to Cook input
status without passing through asset read or write classifications. Cancellation, input write conflicts, file IO,
file/aggregate byte and package-count limits, unknown packages, undeclared
values and missing readers have typed codes with owned identities and limits.
File failures retain the full IO operation, native error, path, offset and size.
Declaration validation uses distinct count, name, duplicate, package, external
file, value, retained-storage and reserved-kind errors. These own the declaring
package, kind/name, file and applicable byte/count bounds. Invalid package paths
retain their complete `FObjectPathError`; payload-shape rejection remains separate
from path parsing. Declaration failure prevents object preparation and output
publication, while an earlier retained cause remains valid across later runs.
Dependency discovery has no string-based failure entrypoint. Root counts and
classes, runtime edge limits, Bulk size/digest mismatch,
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
Family preparation returns `FCookContributionResult`, retaining object/virtual
paths, target settings and preparation diagnostics. The coordinator retains the
complete `ContributionCause`, package and family identity before formatting the
run presentation. Starting another run clears this prior failure state.
Metadata admission rejects explicit roots with no runtime exports and runtime
hard references to excluded source-only objects before any CookState reuse.
Source-only objects and their descendants are omitted from runtime exports;
their bytes, schema and transitive build inputs still invalidate consumers.
EditorOnly references are stripped normally; diagnostic retention writes null
for an excluded target without promoting that target into runtime output.
Live capture applies the same policy and validates hard references with field
and object diagnostics. CoreDObject accepts the generic filter without depending
on Engine asset classes.

Coordinator capture failures retain `FCookCaptureResult` in `CaptureCause`.
Finalization errors own the complete `FCookPlanError`, including nested asset or
path-resolution causes; successful finalization with an unexpected plan count
retains actual and expected counts instead. Both carry package and serializer
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
while package identity, serializer, stage and byte counts remain independent.
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

The package publisher uses the stable `engine-package` identity and its own
serializer version (currently 4). Native class schemas own behavior versions.
CookState uses `Serializer`/`SerializerVersion`; there is no retained contributor
or family-producer compatibility word. Only the current state format is decoded;
older state is discarded and outputs are recooked. Family recipe, function,
constants and payload versions remain explicit dependency values.

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
