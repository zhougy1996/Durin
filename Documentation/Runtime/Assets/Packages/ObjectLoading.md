# Object Package Loading

Summary: Define load-local schema bindings, override restoration ordering, and buffered object-application diagnostics.
Modules: Engine, CoreDObject

Last reviewed: 2026-10-31

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

Owner-thread asynchronous object application buffers `AssetLoad` diagnostics and
emits them after a load taking at least 16 ms. `ObjectLoadTiming` names the root
request; `ObjectLoadPackage` reports each package's own time, serialized export
count and top-level property tags. Counts exclude package containers and do not
count all recursively nested values.

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
