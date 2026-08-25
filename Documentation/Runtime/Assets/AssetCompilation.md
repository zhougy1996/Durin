# Asset Compilation

Summary: Define the Engine-owned object-aware compilation aggregate and provider lifetime contract.

Modules: Engine, Launch, TextureBuild

Last reviewed: 2026-08-26

`FAssetCompilingManager` is the one process authority for asynchronous asset
compilation. Launch starts it after Core task scheduling and pumps it once per
normal GameThread frame. The aggregate owns domain discovery, dependency
ordering, bounded processing, object routing, aggregate progress, successful
post-compile notification, and shutdown placement. It owns no compiler payload,
DDC key, worker queue, or asset publication policy.

The built-in domain is `Durin.MaterialCompilation`. Optional modules register
additional domains while the aggregate is accepting requests;
`TextureBuild` contributes `Durin.TextureCompilation`. Runtime Engine does not
depend on Developer or Editor providers, and game deployments do not acquire a
TextureBuild or DDC dependency through the aggregate.

## Domain Contract

Each `IAssetCompilingManager` publishes one canonical unique domain name and
zero or more dependency-domain names. Missing dependencies are diagnosed but
permitted for optional modules. Cycles among live domains reject the new
registration without changing the prior order. Independent domains use their
canonical names for deterministic ordering.

Dependency-first order applies to normal processing, selected-object finish,
and finish-all. Shutdown first closes every domain's admission, then finishes
and shuts domains down in reverse dependency order. Provider calls and
post-compile listener dispatch never hold the aggregate registry mutex.

Normal processing has a process completion limit of 64. The aggregate gives
each ready domain a bounded first opportunity, rotates independent peers, and
reclaims quota unused by idle domains. Concrete managers report consumed
completions separately from successfully published live objects.

## Object Operations and Publication

Selected finish and cancellation broadcast a bounded `DObject*` span to every
domain in dependency order. Each domain filters the object families it owns.
Cancellation is advisory and does not imply quiescence; a caller requiring an
asset-visible terminal state follows it with selected finish. Finish-all is
reserved for actual global barriers.

Concrete managers retain weak, generation-qualified object identity and admit
results only on GameThread after their family-specific revisions, target, and
dependency qualifiers still match. Workers receive detached immutable values
and never resolve or mutate managed objects. A successful, current publication
is returned to the aggregate as a weak object identity. Failed, canceled,
superseded, destroyed, and stale results do not emit success.

The aggregate coalesces duplicate successful object reports per domain and
broadcasts `FAssetPostCompileData` outside provider calls and the registry
mutex. Listeners may submit future work, but event dispatch does not recursively
enter a provider callback.

## Module and Task Lifetime

External registration returns a move-only
`FAssetCompilingManagerRegistration`. Registration retains a provider resource
lease for its full lifetime and enters the provider's
`FModuleOwnedCallbackGate` immediately before every callback. Reset removes the
domain from future snapshots, stops admission, finishes accepted work, invokes
provider shutdown, destroys retained provider values, and only then releases
the module resource lease. Owner retirement rejects later callback entry.

Concrete domains retain their own task scopes, cancellation sources,
concurrency and memory bounds, mailboxes, diagnostics, and timeout policy. The
aggregate does not add a compilation thread pool. Process shutdown completes
the aggregate before Core closes task admission.

## Initial Domains

Material compilation remains Engine-owned. It preserves program-identity
single-flight sharing, retained program results, last-known-good visibility,
generation-safe admission, Renderer publication, reload behavior, and cooked
program rules. Remaining count is the number of live outstanding material
consumers rather than shared worker flights.

Texture compilation remains TextureBuild-owned. It preserves
`FTexture2DBuildCoordinator` worker admission, priority fairness, memory budget,
DDC behavior, cancellation, completion mailbox, latest-wins authoring state,
and exactly-once completion callbacks. Remaining count is live Texture2D
authoring consumers. TextureCube, VolumeTexture, and Geometry recipes stay
synchronous and do not register empty compilation domains.

## Related Documentation

- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Runtime Lifecycle](../Core/RuntimeLifecycle.md)
- [Texture System](../Rendering/TextureSystem.md)
- [Material System](../Rendering/MaterialSystem.md)
- [Modular Features And Module Retirement](../Core/ModularFeaturesAndModuleRetirement.md)
