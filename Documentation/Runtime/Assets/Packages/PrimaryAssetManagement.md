# Primary Asset Management

Summary: Define metadata-only primary discovery, logical identity redirects, manager ownership, and asynchronous bundle transitions.

Modules: CoreDObject, AssetRegistry, Engine, Launch

Last reviewed: 2026-11-10

## Identity And Discovery

`FPrimaryAssetId` consists of an `FPrimaryAssetType` and asset name, both FName
identities. Text uses exactly one colon with nonempty components: `Type:Name`.
IDs are logical names, not file paths or stable GUIDs. Renaming a logical name
requires an explicit ID redirect. Relocating a package while retaining the ID
changes its discovered path. Deletion and save publication invalidate the
derived manager map on the next catalog revision.

`DObject::GetPrimaryAssetId` returns an invalid ID by default. `DLevel` uses
`Map:<asset name>`. `DPrimaryDataAsset` uses the native reflected class name and
asset name. Native subclasses override `UpdateAssetBundleData` to publish exact
top-level asset paths grouped by bundle name. Bundle names and paths are sorted
and deduplicated; malformed, duplicate, or noncanonical wire entries fail reads.

Discovery reads the persistent `FAssetData` projection, never constructs assets,
and never scans live objects to infer missing IDs. DAST v11 registry-v4 records
contain version-1 optional management metadata. DAST v10 registry-v3 records
have no primary ID or bundles. Ordinary saving upgrades to v11; reads and
source-preserving mutations continue to support v10. Registry cache schema 7
rebuilds older derived caches. Management data is separate from serialized
hard/soft dependency edges.

Duplicate IDs invalidate that ID and report both exact asset paths. Discovery
does not choose an arbitrary winner. ID resolution applies type redirects,
then full-ID redirects, with cycle detection and a 32-step bound. Missing targets
fail loading. Redirects do not create asset metadata.

## Settings And Engine Lifetime

`FProjectAssetManagerSettingsStore` reads the `AssetManager` section of
`Configs/Project.yaml`. Game settings updates preserve it. With no Types filter,
all valid published primary IDs are discoverable. Configured native types
restrict discovery by type, base-class inheritance, directories and exact assets.
Blueprint discovery settings are rejected.

```yaml
AssetManager:
  Class: Durin::DAssetManager
  DefaultRules: {Priority: 0, ChunkId: -1, ApplyRecursively: true, CookRule: Unknown}
  Types:
    - Type: Map
      BaseClass: Durin::DLevel
      Directories: [/Game]
      EditorOnly: false
      Rules: {Priority: 1}
  TypeRedirects:
    - {From: OldMap, To: Map}
  IdRedirects:
    - {From: 'Map:OldName', To: 'Map:NewName'}
  AssetRules:
    - Id: Map:NewName
      Rules: {Priority: 0}
```

Rules default to Priority 0, ChunkId -1, recursive application true and Unknown
Cook rule. Project defaults precede non-default type fields. Per-ID optional
fields can explicitly reset defaults. Supported Cook values are Unknown,
NeverCook and AlwaysCook; distribution-specific values fail settings admission.
These are policy inputs; Cook enforcement is not part of the loading API.

After project modules, reflection and the catalog are ready, `DEngine` resolves
the exact configured native manager subclass, validates inheritance, constructs
it and calls `StartInitialLoading`. Engine GC tracing owns this object. It is
separate from pre-CDO `InitializeAssetRuntime`. Engine shutdown cancels manager
requests and detaches callbacks before retiring state.

## Loading And Bundle State

The manager's streamable requests use the existing shared package backend.
`LoadPrimaryAsset(s)` replaces the desired bundle set. Bundle changes add/remove
named sets and can remove all existing bundles. Targets include the primary
asset and only declared bundle paths, plus their normal hard dependencies.
Unlisted soft references and recursive primary-to-primary bundle expansion are
not loaded automatically.

Each ID has committed and pending bundle sets plus a monotonically increasing
generation. Previous committed roots remain usable during a transition. A
successful complete replacement commits new roots; a failed or canceled request
preserves previous roots. Superseded callbacks cannot overwrite newer state,
including unload followed by a fresh load. A batch reports failures while
successful primaries can commit independently.

Returned handles are observations with their own retention. Releasing them
does not unload manager state. Explicit cancellation cancels still-current
pending transitions; it cannot undo already committed work. A no-work request
returns null and calls completion immediately. Other completion runs through
the normal deferred streamable pump.

`UnloadPrimaryAsset(s)` removes only manager ownership, without forcing GC.
Caller streamable handles, native strong pointers, reflected owners and editor
roots retain their independent lifetimes. Repeated loads do not create leases
or per-caller reference counts. Features needing independent ownership use local
streamable requests or their own coordination policy.

The default game map uses primary preloading when persisted metadata supplies
an ID. Map change waits for that preload and retains its existing local Level
request. After map completion it unloads primary state; the World owns Level
residency. Arbitrary editor previews and thumbnails retain local requests.

## UE Compatibility Boundary

The identity hook, manager lifetime, independent primary retention and named
bundle semantics follow UE's [Asset Management](https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-management-in-unreal-engine)
and [LoadPrimaryAssets](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UAssetManager/LoadPrimaryAssets)
contracts. Durin uses native reflected classes, YAML settings, explicit native
bundle publication and its own versioned package format. Blueprint discovery,
automatic UPROPERTY AssetBundles metadata harvesting, recursive bundles and
chunk/install delivery are outside this supported subset. The per-primary
failure-preserving transition policy is explicit; no UE source-level parity
claim is made for cancellation races or partial failure internals.

See [Asset Streaming](AssetStreaming.md) for handle behavior and
[Asset Residency](AssetResidency.md) for independent runtime/editor roots.
