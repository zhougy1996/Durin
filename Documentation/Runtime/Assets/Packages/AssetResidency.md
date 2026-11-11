# Asset Residency

Summary: Runtime object ownership, editor Standalone flags and checked package unloading.
Modules: CoreDObject, Engine, Launch, AssetTools, AssetForgeBuiltins

Last reviewed: 2026-11-11

## Runtime ownership

A synchronous load returns a borrowed pointer. Strong reflected or native references
and active streamable handles retain objects across garbage collection. An object
request retains its target. A package request retains its top-level exports for
the lifetime of the handle. Soft references and package lookup are non-owning.
Releasing the last owner allows a later collection.

A reachable object keeps its Outer alive. A package indexes its direct exports
for lookup and serialization. Unreferenced sibling exports can be collected
while another export keeps the package alive. Collection removes dead exports
from the index. Loading a partially collected package reconstructs missing
exports, preserves existing object identities and leaves their edited values
intact. Package capture requires a fully loaded package.

Dirty and NewlyCreated describe authoring state. Callers that need a runtime
object across collection must own it, including while editing unsaved data.

## Editing retention

Launch selects the Editor residency policy for the editor host. The default
editor factory flags include Standalone. Editor loading and publication set
Standalone on persistent top-level assets outside PIE. The GC scheduler supplies
Standalone as a keep flag in the editor. A game collection uses NoFlags.
Private prepared graphs are pinned by the operation that owns them. Replacement
transfers Standalone from each previous object to its mapped replacement.

## Checked unloading

AssetTools exposes `PackageTools::UnloadPackages` for a batch of live packages.
The default policy skips dirty or newly created packages. An empty request succeeds. For a nonempty request, true means at least one
eligible package went through unloading, including when individual objects survive. The error string describes skipped
or still-referenced packages. An explicit discard permits unloading unsaved data.

Unloading temporarily clears Standalone on objects in the selected packages and
runs one collection. After reachability analysis and before purge, each reachable
object recovers its original Standalone flag.
An externally owned export can survive even when its siblings are collected.
The collection can also retire unrelated, unowned runtime assets.

Private Cook and reload transactions track exact package generations with weak
references and apply their saved-dependency checks to retryable rollback. Failed
loading withdraws failed new registrations before collection so the path can be
retried. A failed attempt to restore a partial package preserves its prior exports.

The object keep flags and editor batch interface follow the UE 5.8 contracts in
`CoreUObject/Public/UObject/GarbageCollection.h` and
`UnrealEd/Private/PackageTools.cpp`. Durin uses its native DAST format, loader
and purge implementation. Request state is described in [Asset Streaming](AssetStreaming.md).
