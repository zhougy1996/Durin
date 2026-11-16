# Object Package Loading

Summary: Define object preload ownership and bindings, load-local schema bindings, override restoration ordering, and diagnostics.
Modules: Engine, CoreDObject

Last reviewed: 2026-11-17

## Binding ownership

Engine normalizes serialized names before creating one `FLoadSchemaBindings` for
the normalized linker. Schema validation, property application, compatibility
evidence and replacement candidate construction reuse the same field binding.
Bindings include descriptor identity, logical kind/signature, current/deprecated/
removed/incompatible route and optional reflected property. Object consumed bits,
container positions and values remain local to each archive traversal.

The context borrows immutable linker tables and owner-thread reflection metadata.
It is destroyed with the load application and never becomes a global property
cache. Reuse requires the same schema descriptor and current named metadata owner
with matching managed identity. Replaced metadata rejects the binding. Hand-built
metadata lacking managed identity is re-resolved instead of borrowing that proof.
Separate standalone reads and reloads create separate contexts. Cooking/filtering
policy remains with the consuming traversal rather than cached object facts.

Ordinary loose-package loading decodes and validates detached tables once. When
external bulk storage is present, the codec collects resource entries directly
from those tables and synchronously binds the resource before constructing any
objects. It does not project a full inspection or decode the package a second
time for resource registration. Binding failure aborts before skeleton creation;
later load failure retains the ordinary resource-retirement ownership.

## Loading failure ownership

Each `FPackageLinkerLoader` owns its package tables, export records, object table,
load bindings, schema bindings, compatibility evidence and one
`FPackageLoadContext`. The enclosing operation pins the managed objects. Linker validation,
export skeleton construction, dependency traversal, preload field application and
authored-override restoration record the first owned failure in that context.
These internal phases return void. `SerializeExport` invokes `Serialize` or
`SerializeCooked` through `FPackageLoadArchive`; archive failures are captured
before the archive is destroyed. Restoration callbacks run only after successful
field consumption and report rejection through the same context. Failed contexts
skip subsequent export reads and callbacks. Separate or nested package operations
have separate contexts; no global last-error state is used.

Archive diagnostics retain the original failure identity, object path, field route
and archive code. Resolver failures retain their dependency context. Export
restoration failures also acquire the current object path.
Independent codec/schema operations and external dependency resolvers may retain
their value-returning result contracts; the consuming phase records their failure
once. Nested Preload failure only marks its calling archive as failed; the load
context retains the producer's owned diagnostic. Completion uses that same bound
context and skips error reconstruction or formatting when it already failed.
The archive's immediate error text for this case is only a status message; the
operation result carries the full producer diagnostic.
The ordinary linker boundary returns its existing detailed load result, and
private batch preparation returns its existing reason and owned cause.

Preload restores object values and authored intent; graph validation and PostLoad
remain separate. Phase boundaries stop on failure before graph validation,
notification or publication. Existing scoped rollback retires failed ordinary candidates and
private candidate ownership discards incomplete prepared graphs. Serializer
writes are not individually rolled back. The serializer/restoration callback
boundary catches exceptions once, retains the original message and object identity,
and returns through normal failure cleanup. An existing Archive failure takes
precedence over a later serializer exception. Once a first failure is retained,
the catch boundary returns without constructing a replacement exception error.
Field and container scope exits
compare the active exception depth with the archive's entry depth. During new
exception unwinding they only balance traversal scopes; they do not generate
unconsumed-value diagnostics that could replace the callback exception. Normal
scope exits retain strict consumption validation, including when loading was
entered from a caller that was already unwinding. If a serializer swallows an
exception after a stored value scope unwinds, completion rejects serializer
recovery and skips authored restoration. This conservative rule also applies
when that value or container was fully consumed before throwing; it records scope
unwinding rather than proving unread data. Partial values cannot be published.
Preload dispatch and export traversal
do not catch, rethrow or wrap that failure again. Constructor, validator and
PostLoad exceptions retain their enclosing operation boundaries.

## Object preload

`FPackageLinkerLoader::Preload(DObject*)` is the owner-thread, per-object loading
entry point. The loader owns the package application and its fixed field and
authored-override restoration behavior. It has no configurable preload/restore
callbacks or separately borrowed application object. Native class metadata is
already available; it requires no dynamic class-export preload. Skeletons,
template/default initialization, custom versions and schema bindings are ready
before exports are attached and before the skeleton is exposed to recursive loads.

One temporary object annotation records the owning loader and export index. Both
ordinary recursive loading and private preparation use this same binding, without
routing scopes, admission-mode TLS or a second object index. The loading operation
still owns dependency admission, exact private reference resolution and candidate
visibility; a binding only answers which loader owns an already-held object.
`FArchive::Preload(DObject*)` reaches the owning loader through that annotation.
Native, transient, completed and detached objects without an active binding have
no preload work. Preload never discovers a package or starts an implicit public
load. Reading an object reference alone continues to bind a skeleton; it does not
automatically preload every referenced object.

Each export has only a `bNeedsLoad` flag, cleared before following its export Outer
or calling its serializer. Repeated and recursive requests therefore do not
serialize again, matching UE's NeedLoad cycle break. A recursive peer can still
be partly restored; the cleared flag does not promise complete values. Existing
objects excluded from restoration begin with no pending load work. Package
completion is established by returning from the synchronous export traversal
without a recorded failure; there is no separately exported state query, Failed
state or redundant completion scan.

Foreign-object preloads dispatch directly to the recorded loader/export and
preserve its first failure. The owning loader detaches its annotations before
ordinary rollback callbacks and before private candidate retirement, including
exception unwinding.
Private preparation attaches every prepared loader before any export is read;
its candidate graphs remain hidden from public asset lookup. Failed loaders do
no further work and newly created loaders on the same identities can retry.

The load service still coordinates dependency-component validation, PostLoad and
publication after all member packages finish their export traversal. Private
preparation restores the complete batch before graph validation and never runs
candidate PostLoad. Neither a cleared NeedLoad flag nor a nested preload permits
publication of an incomplete component.

## Value and intent publication

Import/reference bindings are selected for the package before value application.
Each export loads through `FPackageLoadArchive`, preserving per-instance field
consumption and container validation. Struct PostDeserialize finishes within
value traversal. After successful consumption, a synchronous callback records
deprecated-field metadata and restores authored intent before returning from the
load archive. No external callback runs between eligible observations and their
publication.

Eligible reflected traversal uses the restricted one-shot restoration protocol;
unproven object/Struct serializers and filtered contexts retain the strict
Discovery/AuthoredPackage validator. Engine owns Forced-tag interpretation,
aliases, deprecated routing and version policy. CoreDObject owns pointer-free
state, eligibility proof and atomic publication. Neither ordinary Explicit tags
nor default differences create replacement intent. See
[authored property overrides](../../ObjectSystem/PropertyOverrides.md).

Recoverable gates precede graph validation and PostLoad notifications. Dependency
SCC validation, private graph preparation, cancellation and replacement rollback
retain their existing ownership. PostLoad migrations may explicitly change intent;
no cached restoration runs after them. Failed candidates release observations,
bindings and private graphs; previously published replacement graphs remain intact.

## Object-load timing and work

With `--startup-diagnostics`, owner-thread asynchronous object application buffers `AssetLoad` diagnostics and
emits them after a load taking at least 16 ms. `ObjectLoadTiming` names the root
request; `ObjectLoadPackage` reports each package's own time, serialized export
count and top-level property tags. Counts exclude package containers and do not
count all recursively nested values.

Collection is disabled by default. Disabled sessions do not read their clock,
copy the root identity, allocate package-statistic storage, enumerate exports for
diagnostic counts, or collect binding/strict-restoration work counters. Ordinary
load validation and publication are unchanged. The process flag and retained
Tracy zones are described in [CPU profiling](../../../Development/Build/Profiling.md#startup-and-load-diagnostics).

`ObjectLoadPhase` retains loading landmarks including `CreateExports`, `Preload`,
`SerializeExports`, `ResolveImports`, `ResolveReferences` and `PostLoad`, plus
schema, initialization and the historical `BuildOverrideLedger`/
`ApplyOverrideLedger` phase names. These phase names preserve diagnostic continuity
after the implementation moves behind the manager. The synchronous restore callback
is a child of serialization; additive exclusive work remains comparable.

`inclusive_ms` includes children; `dependency_ms` includes nested other-package
work; `self_ms` excludes dependency work. `exclusive_ms` excludes same-package
children too and is the only additive measure. Per-phase `object_visits`/
`property_visits` count attributed work, not unique objects or fields. Deferred
graph validation and PostLoad retain their package identity.

`ObjectLoadBindings` reports binding builds/reuses. `ObjectLoadOverrideRestore`
reports fast/fallback objects, full captures actually attempted by strict
restoration and path-token visits. Fast restoration performs no such captures.
`ObjectLoadOverrideFallback` reports counts by `EOverrideRestoreFallbackReason`;
diagnostics are bounded per package/reason, without per-field logging or a second
object walk. Capture counts exclude default planning, duplication and other work
outside restoration. Empty strict restoration requires no captures.

Timings are diagnostic unless collected in a comparable quiet qualification lane.
These diagnostics do not change scheduling, move work beyond Ready, or establish
a performance improvement from one sample.
