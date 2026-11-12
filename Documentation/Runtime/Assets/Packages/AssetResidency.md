# Asset Residency

Summary: Runtime object ownership, editor Standalone flags and checked package unloading.
Modules: CoreDObject, Engine, Launch, AssetTools, AssetForgeBuiltins

Last reviewed: 2026-11-12

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

AssetTools exposes the abstract reflected `DPackageTools` facade in `PackageTools.h`.
Its native static overloads accept a package span, a span/error/dirty policy, or
`DPackageTools::FUnloadPackageParams` with an owned package list, dirty policy,
transaction-buffer reset option and output error. Span-based functions are not
registered as scripting functions. The default skips dirty/new packages and
resets the active editor transaction buffer before collection. Reset rejection
aborts collection; disabling reset preserves history references and may prevent
unloading. Skipped packages and exports are retained through collection.

True means at least one candidate package actually left the resident set. Empty,
invalid, dirty-only and wholly referenced requests return false; partial success
returns true while retaining unsaved/reference diagnostics. This follows UE's
stated result meaning; unlike UE 5.8's empty-array early return, an empty Durin
request reports no change. An explicit discard permits unsaved data loss.

Engine's public `Asset/PackageCollection.h` supplies `CheckPackageCollectionAllowed`
and `CollectPackageExports`. The latter rechecks owner-thread/load/GC admission,
accepts weak package-instance identities, ignores stale/null/nonresident entries,
deduplicates candidates and returns admission, actual collected count and
referenced candidate paths. It applies no dirty-state or transaction policy.
Only Engine owns flag mutation, GC, package-resource retirement and soft-cache
invalidation. AssetTools and native test support require no Engine private
include directory. Runtime release retains its saved-dependency policy and uses
the same private collection algorithm.

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
