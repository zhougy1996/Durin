# Asset Compilation

Summary: Define the Engine-owned object-aware compilation aggregate, class routing, and compiler lifetime contract.

Modules: Engine, Launch, TextureBuild

Last reviewed: 2026-09-02

`FAssetCompilingManager` is the one process authority for asynchronous asset
compilation. Launch starts it after Core task scheduling and pumps it once per
normal GameThread frame. The aggregate owns compiler registration, reflected-class
routing, bounded processing, aggregate progress, successful
post-compile notification, and shutdown placement. It does not impose one
typeless compiler payload, DDC key, queue, or result-application policy;
each Engine-owned typed manager retains its values and invariants.

The built-in compilers are `Durin.Material`, routed from `DMaterial`, and
`Durin.Texture`, routed from `DTexture2D`. Optional modules may register additional
compilers and class routes while the aggregate is accepting requests. Runtime Engine does not require
TextureBuild or DerivedDataCache in Game: editor-enabled Engine optionally links
DDC, and authoring targets register a synchronous
family-specific `ITexture2DBuildProvider`, `IVolumeTextureBuildProvider`, and
`ITextureCubeBuildProvider` features, while game deployments retain Engine
runtime assets and simply have no provider-backed authoring work to submit.

## Compiler Registration Contract

Each `FAssetCompilingManagerRegistration` supplies one canonical unique compiler
name, one `IAssetCompilingManager`, and one or more unique non-null reflected
asset classes. Compiler names identify diagnostics and post-compile events;
they do not route objects. Two live registrations cannot claim the same exact
class. Duplicate compiler names, duplicate submitted classes, and exact-class
conflicts reject the complete registration without publishing any route.

One compiler may own several root classes without adding another lifecycle
entry. Independent compilers use canonical-name order; shutdown first closes
every compiler's admission, then finishes and shuts them down in reverse order. Compiler calls and
post-compile listener dispatch never hold the aggregate registry mutex.

Normal processing has a process completion limit of 64. The aggregate gives
each ready compiler a bounded first opportunity, rotates peers, and
reclaims quota unused by idle compilers. Concrete managers report consumed
completions separately from successfully published live objects.

## Object Operations and Result Application

Selected finish and cancellation resolve each valid object's exact `DClass`, then
walk its superclass chain to the nearest registered route. Resolved objects are
grouped by compiler while retaining their per-compiler input order, and each
compiler is invoked once with only its group. Unregistered objects are no-ops.
Cancellation is advisory and does not imply quiescence; a caller requiring an
asset-visible terminal state follows it with selected finish. Finish-all is
reserved for actual global barriers.

Concrete managers retain generation-safe object handles and independently named
request serial, authored/build identity, target, and dependency qualifiers.
They admit results only on GameThread after every family-specific qualifier
still matches. Workers receive detached immutable values and never resolve or
mutate managed objects. A successful, current result application is returned to the
aggregate as a weak object identity. Failed, canceled, superseded, destroyed,
and stale results do not emit success.

The aggregate coalesces duplicate successful object reports per compiler and
broadcasts `FAssetPostCompileData` outside compiler calls and the registry mutex.
Listeners may submit future work, but event dispatch does not recursively enter
a compiler callback.

## Module and Task Lifetime

External registration returns a move-only
`FAssetCompilerRegistrationHandle`. Registration retains a compiler-owner
resource lease for its full lifetime and enters the compiler owner's
`FModuleOwnedCallbackGate` immediately before every callback. Reset removes the
compiler and its routes from future snapshots, stops admission, finishes accepted work, invokes
compiler shutdown, destroys retained values, and only then releases
the module resource lease. Owner retirement rejects later callback entry.

Concrete managers retain their own Engine task scopes, cancellation sources,
concurrency and memory bounds, mailboxes, diagnostics, and timeout policy. The
aggregate does not add a compilation thread pool. Process shutdown completes
the aggregate before Core closes task admission.

Provider modules do not own those scopes or return concrete asynchronous tasks.
Engine enters the single family-specific Texture provider modular feature for
one synchronous value-only call. Only Texture2D places that call on an
Engine-owned worker; VolumeTexture and TextureCube remain synchronous.
Provider owner retirement closes
new admission and waits for calls already inside the feature gate; no provider
callback, task, deleter, or result lifetime escapes the call.

## Initial Compiling Managers

Material compilation remains Engine-owned. It preserves program-identity
single-flight sharing, retained program results, last-known-good visibility,
generation-safe admission, Renderer publication, reload behavior, and cooked
program rules. Remaining count is the number of live outstanding material
consumers rather than shared worker flights.

Texture compilation is Engine-owned. One `FTextureCompilingManager` owns
typed asset state, worker admission, priority fairness, memory budget,
cancellation, the completion mailbox, latest-wins request serials, GameThread
completion application, and exactly-once completion callbacks. Active records are keyed by
`FObjectHandle` and erased at terminal delivery; retained work diagnostics are
bounded independently. `DTexture2D` owns only its process-local request serial
and last-request diagnostic handle. The deterministic input/provider identity
remains separate from that serial and from GPU resource readiness.

TextureBuild owns the three synchronous pure provider implementations, build
algorithms, recipe metrics, and producer versions. Editor-enabled Engine owns
Texture DDC keys, Get/Put, PlatformData validation and serialization, live
Texture objects, authored state, PostLoad orchestration, diagnostics, completion
application, and resource invalidation.
TextureCube and VolumeTexture stay synchronous and do not register class routes.

## Proven Reuse Boundary

Material and Texture2D prove a common lifecycle shape, not a common compiler
state object. Reuse stays at these Engine-owned boundaries:

| Stable boundary | Shared rule |
| --- | --- |
| Aggregate/compiler contract | Stop admission, process bounded completions, route object operations, finish accepted work, then shut down. |
| Object identity | Carry an `FObjectHandle`; never use an asset path as live-object identity. |
| Freshness | Carry an independently named per-object completion epoch. Material uses authored/dependency revisions and generation; Texture2D uses request serial plus deterministic input/provider identity. |
| Detached completion | Workers produce family-owned value envelopes; only the GameThread resolves the owner and attempts result application. |
| Cancellation and terminal delivery | Cancellation is advisory, late results are consumed, and every accepted consumer reaches one typed terminal outcome. |
| Lifetime accounting | Active records end with terminal delivery; only explicitly bounded diagnostics or family caches may remain. |

These are contract conventions and existing Core/Engine primitives, not a new
typeless job framework. Material retains program-identity single-flight,
multiple consumers, retained programs, last-known-good behavior, Renderer
publication, and its authored/dependency checks. Texture2D retains its priority
queue, byte budget, synchronous provider/DDC boundary, mutation-aware CPU
payload result application, and separate GPU resource enqueue. Their managers remain
typed because a shared state bag would hide rather than enforce those
invariants.

Catalog revision, package format/schema version, object load generation, build
and DDC producer identity, cook target/profile, CPU payload readiness, GPU or
physics resource readiness, mutation transaction state, and
cancellation/shutdown/module-owner state remain orthogonal. Package loading is
not an asset compiler, and Durin has no global composite asset-status
enum. A later Mesh or other asset-family migration requires its own plan once
its owner, producer boundary, publication transaction, and readiness semantics
are known.

## Related Documentation

- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)
- [Texture System](../Rendering/TextureSystem.md)
- [Material System](../Rendering/MaterialSystem.md)
- [Modular Features And Module Retirement](../Core/ModularFeaturesAndModuleRetirement.md)
