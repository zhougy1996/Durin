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

Editor factory defaults include Standalone. Each DAST export records its object's
Public, Standalone, Transactional, ClassDefaultObject and DefaultSubobject flags.
The loader applies those flags to the corresponding export during construction.
DAST v11 export tables imply Public for top-level exports and carry no
Standalone flags. Saving writes DAST v12 with export table version 2. Partial loads
preserve the flags of surviving live objects. Editor GC supplies Standalone as
a keep flag. Game GC uses NoFlags. Private prepared graphs are pinned by their
owning operation.

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
