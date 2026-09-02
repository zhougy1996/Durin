# Asset Compilation

Summary: Define the Engine-owned object-aware compilation aggregate and domain lifetime contract.

Modules: Engine, Launch, TextureBuild

Last reviewed: 2026-09-02

`FAssetCompilingManager` is the one process authority for asynchronous asset
compilation. Launch starts it after Core task scheduling and pumps it once per
normal GameThread frame. The aggregate owns domain discovery, dependency
ordering, bounded processing, object routing, aggregate progress, successful
post-compile notification, and shutdown placement. It does not impose one
typeless compiler payload, DDC key, queue, or publication policy across domains;
each Engine-owned domain retains its typed values and invariants.

The built-in domains are `Durin.MaterialCompilation` and
`Durin.TextureCompilation`. Optional modules may register additional domains
while the aggregate is accepting requests. Runtime Engine does not depend on
TextureBuild or DerivedDataCache: authoring targets register a synchronous
family-specific `ITexture2DBuildProvider`, `IVolumeTextureBuildProvider`, and
`ITextureCubeBuildProvider` features, while game deployments retain Engine
runtime assets and simply have no provider-backed authoring work to submit.

## Domain Contract

Each `IAssetCompilationDomain` publishes one canonical unique domain name and
zero or more dependency-domain names. Missing dependencies are diagnosed but
permitted for optional modules. Cycles among live domains reject the new
registration without changing the prior order. Independent domains use their
canonical names for deterministic ordering.

Dependency-first order applies to normal processing, selected-object finish,
and finish-all. Shutdown first closes every domain's admission, then finishes
and shuts domains down in reverse dependency order. Domain calls and
post-compile listener dispatch never hold the aggregate registry mutex.

Normal processing has a process completion limit of 64. The aggregate gives
each ready domain a bounded first opportunity, rotates independent peers, and
reclaims quota unused by idle domains. Concrete domains report consumed
completions separately from successfully published live objects.

## Object Operations and Publication

Selected finish and cancellation broadcast a bounded `DObject*` span to every
domain in dependency order. Each domain filters the object families it owns.
Cancellation is advisory and does not imply quiescence; a caller requiring an
asset-visible terminal state follows it with selected finish. Finish-all is
reserved for actual global barriers.

Concrete domains retain generation-safe object handles and independently named
request serial, authored/build identity, target, and dependency qualifiers.
They admit results only on GameThread after every family-specific qualifier
still matches. Workers receive detached immutable values and never resolve or
mutate managed objects. A successful, current publication is returned to the
aggregate as a weak object identity. Failed, canceled, superseded, destroyed,
and stale results do not emit success.

The aggregate coalesces duplicate successful object reports per domain and
broadcasts `FAssetPostCompileData` outside domain calls and the registry mutex.
Listeners may submit future work, but event dispatch does not recursively enter
a domain callback.

## Module and Task Lifetime

External registration returns a move-only
`FAssetCompilationDomainRegistration`. Registration retains a domain-owner
resource lease for its full lifetime and enters the domain owner's
`FModuleOwnedCallbackGate` immediately before every callback. Reset removes the
domain from future snapshots, stops admission, finishes accepted work, invokes
domain shutdown, destroys retained domain values, and only then releases
the module resource lease. Owner retirement rejects later callback entry.

Concrete domains retain their own Engine task scopes, cancellation sources,
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

## Initial Domains

Material compilation remains Engine-owned. It preserves program-identity
single-flight sharing, retained program results, last-known-good visibility,
generation-safe admission, Renderer publication, reload behavior, and cooked
program rules. Remaining count is the number of live outstanding material
consumers rather than shared worker flights.

Texture compilation is Engine-owned. One `FTexture2DCompilationDomain` owns
typed asset state, worker admission, priority fairness, memory budget,
cancellation, the completion mailbox, latest-wins request serials, GameThread
publication, and exactly-once completion callbacks. Active records are keyed by
`FObjectHandle` and erased at terminal delivery; retained work diagnostics are
bounded independently. `DTexture2D` owns only its process-local request serial
and last-request diagnostic handle. The deterministic input/provider identity
remains separate from that serial and from GPU resource readiness.

TextureBuild owns the three synchronous provider implementations, build
algorithms, DDC sessions, producer versions, private codecs/helpers, and Build
Function registration. Engine alone owns live Texture objects, authored state,
PostLoad orchestration, diagnostics, publication, and resource invalidation.
TextureCube and VolumeTexture stay synchronous and do not register empty
compilation domains.

## Proven Reuse Boundary

Material and Texture2D prove a common lifecycle shape, not a common compiler
state object. Reuse stays at these Engine-owned boundaries:

| Stable boundary | Shared rule |
| --- | --- |
| Aggregate/domain contract | Stop admission, process bounded completions, route object operations, finish accepted work, then shut down. |
| Object identity | Carry an `FObjectHandle`; never use an asset path as live-object identity. |
| Freshness | Carry an independently named per-object publication epoch. Material uses authored/dependency revisions and generation; Texture2D uses request serial plus deterministic input/provider identity. |
| Detached completion | Workers produce family-owned value envelopes; only the GameThread resolves the owner and attempts publication. |
| Cancellation and terminal delivery | Cancellation is advisory, late results are consumed, and every accepted consumer reaches one typed terminal outcome. |
| Lifetime accounting | Active records end with terminal delivery; only explicitly bounded diagnostics or family caches may remain. |

These are contract conventions and existing Core/Engine primitives, not a new
typeless job framework. Material retains program-identity single-flight,
multiple consumers, retained programs, last-known-good behavior, Renderer
publication, and its authored/dependency checks. Texture2D retains its priority
queue, byte budget, synchronous provider/DDC boundary, mutation-aware CPU
payload publication, and separate GPU resource enqueue. Their managers remain
typed because a shared state bag would hide rather than enforce those
invariants.

Catalog revision, package format/schema version, object load generation, build
and DDC producer identity, cook target/profile, CPU payload readiness, GPU or
physics resource readiness, mutation transaction state, and
cancellation/shutdown/module-owner state remain orthogonal. Package loading is
not an asset-compilation domain, and Durin has no global composite asset-status
enum. A later Mesh or other asset-family migration requires its own plan once
its owner, producer boundary, publication transaction, and readiness semantics
are known.

## Related Documentation

- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)
- [Texture System](../Rendering/TextureSystem.md)
- [Material System](../Rendering/MaterialSystem.md)
- [Modular Features And Module Retirement](../Core/ModularFeaturesAndModuleRetirement.md)
